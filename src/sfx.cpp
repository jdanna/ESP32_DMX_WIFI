#ifdef ANIMATRONIC
#include "sfx.h"

#include <LittleFS.h>
#include <esp_random.h>
#include "usb/uac_host.h"
#include "usb/usb_host.h"
#include "log.h"

namespace {

constexpr uint32_t MAX_OUT_RATE = 96000;
constexpr uint32_t CHUNK_DIV = 100;          // mix 10 ms at a time
constexpr uint32_t UAC_BUFFER = 19200;       // ~100 ms of 48 kHz stereo in the driver's ring buffer
constexpr uint32_t UAC_THRESHOLD = 4800;
constexpr uint32_t PREFERRED_RATES[] = {48000, 44100, 32000, 24000, 22050, 16000};
constexpr uint32_t NO_DEVICE_HINT_MS = 15000;  // nothing enumerated this long after start -> log a hint
constexpr uint32_t REPOWER_FIRST_MS = 3000;     // no device this long after start -> re-power the root port
constexpr uint32_t REPOWER_EVERY_MS = 5000;     // ...and keep trying this often
constexpr uint8_t REPOWER_TRIES = 6;
constexpr uint32_t ERROR_LOG_EVERY_MS = 5000;   // rate limit for repeating stream errors
const char *UPLOAD_PATH = "/sfx_upload.wav";

enum UsbState : uint8_t { USB_OFF, USB_WAITING, USB_CONNECTED, USB_UNSUPPORTED, USB_ERROR };
const char *USB_STATE_NAMES[] = {"off", "waiting", "connected", "unsupported", "error"};

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
volatile bool usbDeviceSeen;
volatile uint32_t xferErrors;  // isochronous transfer failures since the speaker connected
volatile uint32_t toneFrames;  // test tone: frames left to play, bypasses the volume
constexpr uint32_t STREAM_LOG_EVERY_MS = 10000;  // diagnostics: one stream check this long after connect

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
    static uint32_t phase;                            // test tone, 440 Hz at -6 dBFS regardless of volume
    const uint32_t phaseStep = (uint32_t)(((uint64_t)440 << 32) / outRate);
    for (uint32_t i = 0; i < frames; i++) {
      int32_t x = ((int64_t)mix[i] * gain) >> 15;  // four loud voices overflow int32
      if (toneFrames) {
        x += (int32_t)(sinf(phase * (2 * PI / 4294967296.0f)) * 16384);
        phase += phaseStep;
        toneFrames = toneFrames - 1;
      }
      x = constrain(x, -32768, 32767);
      for (uint8_t c = 0; c < outCh; c++) out[i * outCh + c] = x;
    }

    esp_err_t err = ESP_FAIL;
    static uint32_t bytesWritten, statsAt;
    static bool statsLogged, audioLogged;
    uint32_t packets = 0, skipped = 0, busBytes = 0, audioPackets = 0;
    xSemaphoreTake(spkLock, portMAX_DELAY);
    const bool open = spk;
    if (spk) {
      err = uac_host_device_write(spk, (uint8_t *)out, frames * outCh * 2, pdMS_TO_TICKS(200));
      uac_host_device_get_tx_stats(spk, &packets, &skipped, &busBytes, &audioPackets);
    }
    xSemaphoreGive(spkLock);
    if (err == ESP_OK) bytesWritten += frames * outCh * 2;
    if (open && !audioLogged && audioPackets) {
      audioLogged = true;
      logInfo("sfx", "First non-silent packet on the bus (packet %lu)", (unsigned long)packets);
    }
    // One check per connection: the write rate should match the stream rate (e.g. 192 kB/s at
    // 48 kHz stereo) and no packets should be skipped. Faster than real time means data isn't
    // reaching the bus.
    if (open && !statsLogged) {
      if (!statsAt) statsAt = millis();
      if (millis() - statsAt >= STREAM_LOG_EVERY_MS) {
        const uint32_t ms = millis() - statsAt;
        logInfo("sfx", "Speaker stream: %lu kB/s written (expect %lu), %lu packets / %lu bytes on the bus, %lu skipped, %lu with audio",
                (unsigned long)(bytesWritten / ms), (unsigned long)(outRate * outCh * 2 / 1000),
                (unsigned long)packets, (unsigned long)busBytes, (unsigned long)skipped, (unsigned long)audioPackets);
        statsLogged = true;
      }
    }
    if (!open) {
      statsAt = 0;
      bytesWritten = 0;
      statsLogged = audioLogged = false;
    }
    if (err != ESP_OK) {
      // A timeout means the speaker isn't consuming audio: the driver's buffer stays full.
      static uint32_t lastLog, failures;
      failures++;
      if (open && millis() - lastLog >= ERROR_LOG_EVERY_MS) {
        logWarn("sfx", "USB speaker isn't taking audio (%s, %lu failed writes)", esp_err_to_name(err),
                (unsigned long)failures);
        lastLog = millis();
        failures = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
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
  if (event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) xferErrors = xferErrors + 1;  // only this task writes it
  // TX_DONE fires on nearly every transfer; queueing it could crowd out a disconnect.
  if (event != UAC_HOST_DRIVER_EVENT_DISCONNECTED) return;
  UsbEvent e = {true, 0, 0, event, handle};
  xQueueSend(usbEvents, &e, pdMS_TO_TICKS(100));
}

// Runs for every device the host enumerates, audio or not.
bool onEnumerate(const usb_device_desc_t *d, uint8_t *) {
  usbDeviceSeen = true;
  logInfo("usb", "Device %04X:%04X connected (USB %x.%02x, class %u)", d->idVendor, d->idProduct, d->bcdUSB >> 8,
          d->bcdUSB & 0xff, d->bDeviceClass);
  return true;
}

bool supportsRate(const uac_host_dev_alt_param_t &p, uint32_t rate) {
  if (p.sample_freq_type == 0) return rate >= p.sample_freq_lower && rate <= p.sample_freq_upper;
  for (int i = 0; i < p.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++)
    if (p.sample_freq[i] == rate) return true;
  return false;
}

// Picks a 16-bit format, preferring stereo at the most common rates.
bool pickFormat(uac_host_device_handle_t h, const uac_host_dev_info_t &info, uint8_t &channels, uint32_t &rate) {
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

void logFormats(uac_host_device_handle_t h, const uac_host_dev_info_t &info) {
  for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
    uac_host_dev_alt_param_t p;
    if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK) continue;
    char rates[48] = "";
    if (p.sample_freq_type == 0) {
      snprintf(rates, sizeof(rates), "%lu-%lu Hz", (unsigned long)p.sample_freq_lower, (unsigned long)p.sample_freq_upper);
    } else {
      for (int i = 0; i < p.sample_freq_type && i < UAC_FREQ_NUM_MAX; i++)
        snprintf(rates + strlen(rates), sizeof(rates) - strlen(rates), "%s%lu", i ? "/" : "", (unsigned long)p.sample_freq[i]);
      strlcat(rates, " Hz", sizeof(rates));
    }
    logInfo("sfx", "  format %u: %u ch, %u-bit (%u-byte), %s, tag %u", alt, p.channels, p.bit_resolution,
            p.subframe_size, rates, p.format);
  }
}

void openSpeaker(uint8_t addr, uint8_t iface) {
  if (spk) {
    logWarn("sfx", "Ignoring a second USB speaker (address %u)", addr);
    return;
  }
  const uac_host_device_config_t cfg = {
      .addr = addr,
      .iface_num = iface,
      .buffer_size = UAC_BUFFER,
      .buffer_threshold = UAC_THRESHOLD,
      .callback = onDeviceEvent,
      .callback_arg = nullptr,
  };
  uac_host_device_handle_t h;
  esp_err_t err = uac_host_device_open(&cfg, &h);
  if (err != ESP_OK) {
    logError("sfx", "Couldn't open the USB speaker: %s", esp_err_to_name(err));
    usbState = USB_ERROR;
    return;
  }

  uac_host_dev_info_t info = {};
  uac_host_get_device_info(h, &info);
  char product[32];
  size_t n = 0;
  for (; n < sizeof(product) - 1 && info.iProduct[n]; n++) product[n] = info.iProduct[n] < 0x80 ? info.iProduct[n] : '?';
  product[n] = 0;
  logInfo("sfx", "USB speaker \"%s\" (%04X:%04X), interface %u, %u format(s)", product, info.VID, info.PID, iface,
          info.iface_alt_num);
  logFormats(h, info);

  uint8_t channels;
  uint32_t rate;
  if (!pickFormat(h, info, channels, rate)) {
    logError("sfx", "USB speaker has no 16-bit PCM format at a supported rate");
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
  err = uac_host_device_start(h, &stream);
  if (err != ESP_OK) {
    logError("sfx", "USB speaker didn't start at %lu Hz %u ch: %s", (unsigned long)rate, channels, esp_err_to_name(err));
    usbState = USB_ERROR;
    uac_host_device_close(h);
    return;
  }

  // Tell the endpoint its rate even when the descriptor says it has no frequency control.
  // macOS does this unconditionally and some chips (Jieli "UACDemoV1.0") seem to expect it:
  // the stream is otherwise accepted but never reaches the DAC.
  err = uac_host_device_set_ep_frequency(h, rate);
  logInfo("sfx", "Sampling frequency request %lu Hz: %s", (unsigned long)rate, err == ESP_OK ? "accepted" : esp_err_to_name(err));

  // Volume is applied in the mixer, so run the speaker's own control flat out.
  // If the device didn't report a usable volume range the driver can compute a
  // bogus level (silence), so check the result and put the old level back.
  err = uac_host_device_set_mute(h, false);
  if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) logWarn("sfx", "Unmute failed: %s", esp_err_to_name(err));
  int16_t before, after;
  if (uac_host_device_get_volume_db(h, &before) == ESP_OK) {
    err = uac_host_device_set_volume(h, 100);
    if (err != ESP_OK) {
      logWarn("sfx", "Setting speaker volume failed: %s", esp_err_to_name(err));
    } else if (uac_host_device_get_volume_db(h, &after) == ESP_OK) {
      if (after < before) {
        logWarn("sfx", "Speaker got quieter at 100%% (%.1f -> %.1f dB), restoring", before / 256.0f, after / 256.0f);
        uac_host_device_set_volume_db(h, before);
      } else {
        logInfo("sfx", "Speaker volume %.1f dB", after / 256.0f);
      }
    }
  }

  outRate = rate;
  outCh = channels;
  snprintf(deviceDesc, sizeof(deviceDesc), "%lu Hz %s", (unsigned long)rate, channels == 2 ? "stereo" : "mono");
  xferErrors = 0;
  xSemaphoreTake(spkLock, portMAX_DELAY);
  spk = h;
  xSemaphoreGive(spkLock);
  usbState = USB_CONNECTED;
  xTaskNotifyGive(mixerHandle);
  logInfo("sfx", "USB speaker connected, %s", deviceDesc);
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
    logInfo("sfx", "USB speaker disconnected");
  }
}

void usbHostTask(void *) {
  const usb_host_config_t cfg = {
      .skip_phy_setup = false,
      .intr_flags = ESP_INTR_FLAG_LOWMED,
      .enum_filter_cb = onEnumerate,
  };
  const esp_err_t err = usb_host_install(&cfg);
  if (err != ESP_OK) {
    logError("usb", "USB host install failed: %s", esp_err_to_name(err));
    usbState = USB_ERROR;
    vTaskDelete(nullptr);
  }
  logInfo("usb", "USB-C port is now a USB host (serial monitor disabled)");
  xTaskNotifyGive(uacHandle);
  for (;;) {
    uint32_t flags;
    usb_host_lib_handle_events(portMAX_DELAY, &flags);
    if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    if (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE) logInfo("usb", "All USB devices removed");
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
  const esp_err_t err = uac_host_install(&cfg);
  if (err != ESP_OK) {
    logError("sfx", "USB audio driver install failed: %s", esp_err_to_name(err));
    usbState = USB_ERROR;
    vTaskDelete(nullptr);
  }
  // Let the driver's warnings and info (e.g. "not UAC device") reach the Log page;
  // library.json compiles them in.
  esp_log_level_set("uac-host", ESP_LOG_INFO);
  usbState = USB_WAITING;
  logInfo("sfx", "Waiting for a USB speaker");
  const uint32_t started = millis();
  bool hinted = false;
  uint32_t errorsLogged = 0, lastErrorLog = 0, rearmAt = 0;
  uint8_t repowers = 0;
  UsbEvent e;
  for (;;) {
    if (xQueueReceive(usbEvents, &e, pdMS_TO_TICKS(500))) {
      if (!e.device && e.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) {
        openSpeaker(e.addr, e.iface);
        if (usbState == USB_CONNECTED) rearmAt = millis() + 1000;
      }
      else if (!e.device && e.event == UAC_HOST_DRIVER_EVENT_RX_CONNECTED)
        logInfo("sfx", "Ignoring a USB microphone interface (address %u)", e.addr);
      else if (e.device && e.event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) closeSpeaker(e.handle);
    }
    // A device that was already attached at boot (e.g. after a software restart) sometimes
    // doesn't answer the first bus reset ("Root port reset failed" from HUB) and the port then
    // sits disabled until it's unplugged. Cycling the root port makes the stack re-detect it.
    if (!usbDeviceSeen && repowers < REPOWER_TRIES &&
        millis() - started > REPOWER_FIRST_MS + (uint32_t)repowers * REPOWER_EVERY_MS) {
      repowers++;
      if (repowers == 1) logInfo("usb", "No USB device answered yet, re-powering the port (up to %u tries)", REPOWER_TRIES);
      usb_host_lib_set_root_port_power(false);
      vTaskDelay(pdMS_TO_TICKS(300));
      usb_host_lib_set_root_port_power(true);
    }
    if (!hinted && !usbDeviceSeen && millis() - started > NO_DEVICE_HINT_MS) {
      hinted = true;
      // The port has no 5 V output: a bus-powered speaker stays off, and a hub won't
      // attach either, since hubs only connect upstream when they see 5 V from the host.
      logWarn("usb", "No USB device detected on the USB-C port. This board doesn't put 5 V on the port, so the "
                     "speaker needs 5 V injected on VBUS (a powered OTG adapter); a powered hub isn't enough.");
    }
    // Some speaker chips (reports mention Jieli "UACDemoV1.0") seem to want a mute/volume
    // command after audio data is already flowing, so send them again once the mixer is streaming.
    if (rearmAt && (int32_t)(millis() - rearmAt) >= 0) {
      rearmAt = 0;
      xSemaphoreTake(spkLock, portMAX_DELAY);
      if (spk) {
        // Last Mac-mimic for stubborn chips: the Mac idles the stream at alt 0 and opens alt 1 when
        // it starts playing, after mute/volume are already set. Cycle the interface the same way.
        const esp_err_t s0 = uac_host_device_suspend(spk);
        const esp_err_t s1 = uac_host_device_resume(spk);
        const esp_err_t f = uac_host_device_set_ep_frequency(spk, outRate);
        logInfo("sfx", "Cycled the stream interface: alt 0 %s, alt 1 %s, frequency %s", esp_err_to_name(s0),
                esp_err_to_name(s1), esp_err_to_name(f));
        const esp_err_t m = uac_host_device_set_mute(spk, false);
        const esp_err_t v = uac_host_device_set_volume(spk, 100);
        bool muted = false;
        int16_t db = 0;
        const esp_err_t gm = uac_host_device_get_mute(spk, &muted), gv = uac_host_device_get_volume_db(spk, &db);
        logInfo("sfx", "Re-sent unmute (%s) and volume (%s) while streaming; speaker reports mute=%s, %.1f dB",
                esp_err_to_name(m), esp_err_to_name(v), gm == ESP_OK ? (muted ? "on" : "off") : esp_err_to_name(gm),
                gv == ESP_OK ? db / 256.0f : 0.0f);
      }
      xSemaphoreGive(spkLock);
    }
    const uint32_t errors = xferErrors;
    if (errors != errorsLogged && millis() - lastErrorLog >= ERROR_LOG_EVERY_MS) {
      logWarn("sfx", "USB audio transfer errors: %lu", (unsigned long)(errors - errorsLogged));
      errorsLogged = errors;
      lastErrorLog = millis();
    }
  }
}

}  // namespace

void sfxBegin(bool usbAudio) {
  lock = xSemaphoreCreateMutex();
  spkLock = xSemaphoreCreateMutex();
  if (!LittleFS.begin(true)) logError("sfx", "LittleFS mount failed");
  for (int i = 0; i < SFX_COUNT; i++) {
    Sound s;
    String err;
    if (!LittleFS.exists(slotPath(i))) continue;
    if (loadWav(slotPath(i), s, err)) sounds[i] = s;
    else logError("sfx", "Sound %d couldn't be loaded: %s", i + 1, err.c_str());
  }
  xTaskCreatePinnedToCore(mixerTask, "sfx_mix", 4096, nullptr, 6, &mixerHandle, 0);
  if (!usbAudio) {
    logInfo("sfx", "USB audio is off (System page)");
    return;
  }
  usbEvents = xQueueCreate(10, sizeof(UsbEvent));
  // Above the mixer: the mixer holds spkLock for nearly the whole time it streams, and a
  // lower-priority task waiting on that mutex would never get it (FreeRTOS gives no fairness).
  xTaskCreatePinnedToCore(uacTask, "uac_events", 4096, nullptr, 7, &uacHandle, 0);
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

void sfxMoveSound() {
  constexpr uint32_t QUIET_MS = 1000;  // after a sound on move ends, this long before the next
  static uint32_t quietUntil;
  static bool played;
  const uint32_t now = millis();
  if (usbState != USB_CONNECTED || (played && (int32_t)(now - quietUntil) < 0)) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  int loaded[SFX_COUNT], n = 0;
  for (int i = 0; i < SFX_COUNT; i++)
    if (sounds[i].pcm) loaded[n++] = i;
  if (n) {
    const int k = loaded[esp_random() % n];
    voices[k] = {true, 0};
    quietUntil = now + (uint32_t)((uint64_t)sounds[k].frames * 1000 / sounds[k].rate) + QUIET_MS;
    played = true;
  }
  xSemaphoreGive(lock);
}

void sfxTone() {
  if (usbState != USB_CONNECTED) return;
  toneFrames = outRate * 3;  // three seconds
  logInfo("sfx", "Test tone");
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
