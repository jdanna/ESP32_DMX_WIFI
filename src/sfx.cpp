#ifdef ANIMATRONIC
#include "sfx.h"

#include <LittleFS.h>
#include "usb/uac_host.h"
#include "usb/usb_host.h"

namespace {

constexpr uint32_t MAX_OUT_RATE = 96000;
constexpr uint32_t CHUNK_DIV = 100;          // mix 10 ms at a time
constexpr uint32_t UAC_BUFFER = 19200;       // ~100 ms of 48 kHz stereo in the driver's ring buffer
constexpr uint32_t UAC_THRESHOLD = 4800;
constexpr uint32_t PREFERRED_RATES[] = {48000, 44100, 32000, 24000, 22050, 16000};
const char *UPLOAD_PATH = "/sfx_upload.wav";

enum UsbState : uint8_t { USB_OFF, USB_WAITING, USB_CONNECTED, USB_UNSUPPORTED };
const char *USB_STATE_NAMES[] = {"off", "waiting", "connected", "unsupported"};

struct Sound {
  int16_t *pcm;  // mono, in PSRAM
  uint32_t frames, rate;
};

struct Voice {
  bool on;
  uint64_t pos;  // 32.32 fixed-point frame position
};

// Sounds and voices: shared by the mixer task, the engine task and web handlers.
SemaphoreHandle_t lock;
Sound sounds[SFX_COUNT];
Voice voices[SFX_COUNT];
volatile uint8_t volumePct = 80;
volatile bool volumeExternal = false;
uint8_t inputPrev;  // engine task only

// Speaker: spkLock is held around every write and the close, so a disconnect
// can't pull the handle out from under the mixer.
SemaphoreHandle_t spkLock;
uac_host_device_handle_t spk;
volatile UsbState usbState = USB_OFF;
uint32_t outRate;
uint8_t outCh;
char deviceDesc[48];
QueueHandle_t usbEvents;
TaskHandle_t mixerHandle, uacHandle;

String slotPath(int slot) { return String("/sfx") + (slot + 1) + ".wav"; }

// ─── WAV loading ────────────────────────────────────────────────────────

// Parses a PCM16 mono/stereo WAV and leaves `f` at the start of the samples.
bool readWavHeader(File &f, uint16_t &channels, uint32_t &rate, uint32_t &dataBytes, String &err) {
  uint8_t h[12];
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) {
    err = "Not a WAV file";
    return false;
  }
  bool haveFmt = false;
  for (;;) {
    uint8_t c[8];
    if (f.read(c, 8) != 8) {
      err = "WAV file has no audio data";
      return false;
    }
    const uint32_t size = c[4] | (c[5] << 8) | (c[6] << 16) | ((uint32_t)c[7] << 24);
    if (!memcmp(c, "fmt ", 4)) {
      uint8_t fmt[16];
      if (size < 16 || f.read(fmt, 16) != 16) {
        err = "Bad WAV format chunk";
        return false;
      }
      const uint16_t format = fmt[0] | (fmt[1] << 8), bits = fmt[14] | (fmt[15] << 8);
      channels = fmt[2] | (fmt[3] << 8);
      rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
      if (format != 1 || bits != 16 || channels < 1 || channels > 2 || rate < 8000 || rate > 96000) {
        err = "WAV must be 16-bit PCM, mono or stereo, 8-96 kHz";
        return false;
      }
      haveFmt = true;
      f.seek(f.position() + size - 16 + (size & 1));
    } else if (!memcmp(c, "data", 4)) {
      if (!haveFmt) {
        err = "Bad WAV file";
        return false;
      }
      dataBytes = min<uint32_t>(size, f.size() - f.position());
      return true;
    } else {
      f.seek(f.position() + size + (size & 1));
    }
  }
}

// Loads a WAV into a PSRAM buffer, mixing stereo down to mono.
bool loadWav(const String &path, Sound &out, String &err) {
  File f = LittleFS.open(path, "r");
  if (!f) {
    err = "File missing";
    return false;
  }
  uint16_t channels;
  uint32_t rate, bytes;
  if (!readWavHeader(f, channels, rate, bytes, err)) return false;
  const uint32_t frames = bytes / (2 * channels);
  if (frames < 2) {
    err = "WAV file is empty";
    return false;
  }
  int16_t *pcm = (int16_t *)ps_malloc(frames * sizeof(int16_t));
  if (!pcm) {
    err = "Not enough memory for this sound";
    return false;
  }
  if (channels == 1) {
    f.read((uint8_t *)pcm, frames * 2);
  } else {
    int16_t buf[256];
    for (uint32_t done = 0; done < frames;) {
      const uint32_t n = min<uint32_t>(128, frames - done);
      f.read((uint8_t *)buf, n * 4);
      for (uint32_t i = 0; i < n; i++) pcm[done + i] = (buf[2 * i] + buf[2 * i + 1]) / 2;
      done += n;
    }
  }
  out = {pcm, frames, rate};
  return true;
}

void setSound(int slot, const Sound &s) {
  xSemaphoreTake(lock, portMAX_DELAY);
  int16_t *old = sounds[slot].pcm;
  sounds[slot] = s;
  voices[slot].on = false;
  xSemaphoreGive(lock);
  free(old);
}

// ─── Mixer ──────────────────────────────────────────────────────────────

void mixerTask(void *) {
  static int32_t mix[MAX_OUT_RATE / CHUNK_DIV];
  static int16_t out[MAX_OUT_RATE / CHUNK_DIV * 2];
  for (;;) {
    if (usbState != USB_CONNECTED) {
      sfxStop(-1);  // nothing to play on: don't leave sounds stuck "playing"
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    const uint32_t frames = outRate / CHUNK_DIV;
    memset(mix, 0, frames * sizeof(int32_t));

    xSemaphoreTake(lock, portMAX_DELAY);
    for (int k = 0; k < SFX_COUNT; k++) {
      Voice &v = voices[k];
      const Sound &s = sounds[k];
      if (!v.on) continue;
      const uint64_t step = ((uint64_t)s.rate << 32) / outRate;
      for (uint32_t i = 0; i < frames; i++) {
        const uint32_t idx = v.pos >> 32;
        if (idx + 1 >= s.frames) {
          v.on = false;
          break;
        }
        // Linear interpolation; a 15-bit fraction keeps the product inside int32.
        const int32_t a = s.pcm[idx], b = s.pcm[idx + 1], frac = (v.pos >> 17) & 0x7fff;
        mix[i] += a + (((b - a) * frac) >> 15);
        v.pos += step;
      }
    }
    xSemaphoreGive(lock);

    const int32_t vol = volumePct;
    const int32_t gain = vol * vol * 32768 / 10000;  // squared curve feels closer to even loudness steps
    for (uint32_t i = 0; i < frames; i++) {
      const int32_t x = constrain(((int64_t)mix[i] * gain) >> 15, -32768, 32767);  // four loud voices overflow int32
      for (uint8_t c = 0; c < outCh; c++) out[i * outCh + c] = x;
    }

    esp_err_t err = ESP_FAIL;
    xSemaphoreTake(spkLock, portMAX_DELAY);
    if (spk) err = uac_host_device_write(spk, (uint8_t *)out, frames * outCh * 2, pdMS_TO_TICKS(200));
    xSemaphoreGive(spkLock);
    if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─── USB speaker ────────────────────────────────────────────────────────

struct UsbEvent {
  bool device;  // false: driver event (connect), true: device event (disconnect, errors)
  uint8_t addr, iface;
  int event;
  uac_host_device_handle_t handle;
};

void onDriverEvent(uint8_t addr, uint8_t iface, const uac_host_driver_event_t event, void *) {
  UsbEvent e = {false, addr, iface, event, nullptr};
  xQueueSend(usbEvents, &e, 0);
}

void onDeviceEvent(uac_host_device_handle_t handle, const uac_host_device_event_t event, void *) {
  UsbEvent e = {true, 0, 0, event, handle};
  xQueueSend(usbEvents, &e, 0);
}

bool supportsRate(const uac_host_dev_alt_param_t &p, uint32_t rate) {
  if (p.sample_freq_type == 0) return rate >= p.sample_freq_lower && rate <= p.sample_freq_upper;
  for (int i = 0; i < p.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++)
    if (p.sample_freq[i] == rate) return true;
  return false;
}

// Picks a 16-bit format, preferring stereo at the most common rates.
bool pickFormat(uac_host_device_handle_t h, uint8_t &channels, uint32_t &rate) {
  uac_host_dev_info_t info;
  if (uac_host_get_device_info(h, &info) != ESP_OK) return false;
  for (uint32_t want : PREFERRED_RATES) {
    for (int stereo = 1; stereo >= 0; stereo--) {
      for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
        uac_host_dev_alt_param_t p;
        if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK) continue;
        if (p.bit_resolution != 16 || p.subframe_size != 2 || p.channels != (stereo ? 2 : 1)) continue;
        if (!supportsRate(p, want)) continue;
        channels = p.channels;
        rate = want;
        return true;
      }
    }
  }
  return false;
}

void openSpeaker(uint8_t addr, uint8_t iface) {
  if (spk) return;  // one speaker at a time
  const uac_host_device_config_t cfg = {
      .addr = addr,
      .iface_num = iface,
      .buffer_size = UAC_BUFFER,
      .buffer_threshold = UAC_THRESHOLD,
      .callback = onDeviceEvent,
      .callback_arg = nullptr,
  };
  uac_host_device_handle_t h;
  if (uac_host_device_open(&cfg, &h) != ESP_OK) return;

  uint8_t channels;
  uint32_t rate;
  if (!pickFormat(h, channels, rate)) {
    log_w("USB speaker has no 16-bit PCM format");
    usbState = USB_UNSUPPORTED;
    uac_host_device_close(h);
    return;
  }
  const uac_host_stream_config_t stream = {
      .channels = channels,
      .bit_resolution = 16,
      .sample_freq = rate,
      .flags = 0,
  };
  if (uac_host_device_start(h, &stream) != ESP_OK) {
    log_w("USB speaker didn't start");
    usbState = USB_UNSUPPORTED;
    uac_host_device_close(h);
    return;
  }
  // Volume is applied in the mixer, so run the speaker's own control flat out.
  uac_host_device_set_mute(h, false);
  uac_host_device_set_volume(h, 100);

  outRate = rate;
  outCh = channels;
  snprintf(deviceDesc, sizeof(deviceDesc), "%lu Hz %s", (unsigned long)rate, channels == 2 ? "stereo" : "mono");
  xSemaphoreTake(spkLock, portMAX_DELAY);
  spk = h;
  xSemaphoreGive(spkLock);
  usbState = USB_CONNECTED;
  xTaskNotifyGive(mixerHandle);
  Serial.printf("[sfx] USB speaker connected, %s\n", deviceDesc);
}

void closeSpeaker(uac_host_device_handle_t h) {
  xSemaphoreTake(spkLock, portMAX_DELAY);
  const bool ours = h == spk;
  if (ours) spk = nullptr;
  xSemaphoreGive(spkLock);
  uac_host_device_close(h);
  if (ours) {
    usbState = USB_WAITING;
    deviceDesc[0] = 0;
  }
}

void usbHostTask(void *) {
  const usb_host_config_t cfg = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LOWMED,
  };
  if (usb_host_install(&cfg) != ESP_OK) {
    log_e("USB host install failed");
    vTaskDelete(nullptr);
  }
  xTaskNotifyGive(uacHandle);
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
  }
}

void uacTask(void *) {
  ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait for the USB host library
  const uac_host_driver_config_t cfg = {
      .create_background_task = true,
      .task_priority = 5,
      .stack_size = 4096,
      .core_id = 0,
      .callback = onDriverEvent,
      .callback_arg = nullptr,
  };
  if (uac_host_install(&cfg) != ESP_OK) {
    log_e("UAC driver install failed");
    vTaskDelete(nullptr);
  }
  usbState = USB_WAITING;
  UsbEvent e;
  for (;;) {
    if (!xQueueReceive(usbEvents, &e, portMAX_DELAY)) continue;
    if (!e.device && e.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) openSpeaker(e.addr, e.iface);
    else if (e.device && e.event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) closeSpeaker(e.handle);
  }
}

}  // namespace

void sfxBegin(bool usbAudio) {
  lock = xSemaphoreCreateMutex();
  spkLock = xSemaphoreCreateMutex();
  if (!LittleFS.begin(true)) log_e("LittleFS mount failed");
  for (int i = 0; i < SFX_COUNT; i++) {
    Sound s;
    String err;
    if (LittleFS.exists(slotPath(i)) && loadWav(slotPath(i), s, err)) sounds[i] = s;
  }
  xTaskCreatePinnedToCore(mixerTask, "sfx_mix", 4096, nullptr, 6, &mixerHandle, 0);
  if (!usbAudio) return;
  usbEvents = xQueueCreate(10, sizeof(UsbEvent));
  xTaskCreatePinnedToCore(uacTask, "uac_events", 4096, nullptr, 2, &uacHandle, 0);
  xTaskCreatePinnedToCore(usbHostTask, "usb_host", 4096, nullptr, 5, nullptr, 0);
}

void sfxInput(uint8_t mask, bool enabled, uint8_t volume, bool volumeFromInput) {
  volumePct = volume;
  volumeExternal = volumeFromInput;
  const uint8_t rising = enabled ? mask & ~inputPrev : 0;
  inputPrev = mask;
  for (int i = 0; i < SFX_COUNT; i++)
    if (rising & (1 << i)) sfxPlay(i);
}

void sfxPlay(int slot) {
  if (slot < 0 || slot >= SFX_COUNT) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (sounds[slot].pcm && usbState == USB_CONNECTED) voices[slot] = {true, 0};  // restarts if already playing
  xSemaphoreGive(lock);
}

void sfxStop(int slot) {
  xSemaphoreTake(lock, portMAX_DELAY);
  for (int i = 0; i < SFX_COUNT; i++)
    if (slot < 0 || slot == i) voices[i].on = false;
  xSemaphoreGive(lock);
}

const char *sfxUploadPath() { return UPLOAD_PATH; }

bool sfxInstall(int slot, String &err) {
  Sound s;
  if (!loadWav(UPLOAD_PATH, s, err)) {
    LittleFS.remove(UPLOAD_PATH);
    return false;
  }
  setSound(slot, s);
  LittleFS.remove(slotPath(slot));
  LittleFS.rename(UPLOAD_PATH, slotPath(slot));
  return true;
}

void sfxDelete(int slot) {
  if (slot < 0 || slot >= SFX_COUNT) return;
  setSound(slot, {nullptr, 0, 0});
  LittleFS.remove(slotPath(slot));
}

uint8_t sfxPlayingMask() {
  uint8_t m = 0;
  xSemaphoreTake(lock, portMAX_DELAY);
  for (int i = 0; i < SFX_COUNT; i++)
    if (voices[i].on) m |= 1 << i;
  xSemaphoreGive(lock);
  return m;
}

uint8_t sfxVolume() { return volumePct; }
bool sfxVolumeFromInput() { return volumeExternal; }

void sfxStatusJson(JsonObject out) {
  out["usb"] = USB_STATE_NAMES[usbState];
  if (usbState == USB_CONNECTED) out["device"] = deviceDesc;
  out["volume"] = volumePct;
  out["volumeFromInput"] = volumeExternal;
  out["playing"] = sfxPlayingMask();
  out["fsTotal"] = LittleFS.totalBytes();
  out["fsUsed"] = LittleFS.usedBytes();
  JsonArray slots = out["slots"].to<JsonArray>();
  xSemaphoreTake(lock, portMAX_DELAY);
  for (int i = 0; i < SFX_COUNT; i++) {
    JsonObject o = slots.add<JsonObject>();
    o["loaded"] = sounds[i].pcm != nullptr;
    if (sounds[i].pcm) o["seconds"] = (float)sounds[i].frames / sounds[i].rate;
  }
  xSemaphoreGive(lock);
}
#endif  // ANIMATRONIC
