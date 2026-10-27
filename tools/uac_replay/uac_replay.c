// Replays the animatronic firmware's USB audio sequence from a Linux PC, to test a
// Jieli "UACDemoV1.0"-style USB speaker without the ESP32 in the loop.
//
// DO NOT RUN ON macOS: detaching Apple's USB audio driver from a live speaker
// kernel-panicked a Mac (IOUSBHostFamily, 2026-10-27). Linux is safe: snd-usb-audio
// is detached while the tool runs and reattached when it exits.
//
// Build: sudo apt install libusb-1.0-0-dev && cc -O2 -o uac_replay uac_replay.c -lusb-1.0 -lm
// Run:   sudo ./uac_replay [esp|mac|noctl|reset-esp] [VID:PID]
//   esp       firmware order: SET_INTERFACE alt 1, sampling freq, unmute, volume, then audio
//   mac       class requests first, then SET_INTERFACE alt 1 and freq, then audio
//   noctl     SET_INTERFACE alt 1 and audio only, no class requests
//   reset-esp USB reset first (fresh device state, like a reseat), then "esp"
// Default devices: 4c4a:4155 and 5a47:5858. Assumes the Jieli template layout:
// AudioControl interface 0, Feature Unit 2 (mute on master, volume on ch 1-2),
// streaming interface 1 alt 1, isochronous OUT endpoint 0x03, 48 kHz 16-bit stereo.
#include <libusb-1.0/libusb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#error "Not for macOS: detaching the macOS USB audio driver can kernel panic. Use Linux."
#endif

#define PKT 192   // 48 kHz * 2 ch * 2 bytes / 1000
#define PKTS 8    // packets per transfer
#define XFERS 8
#define SECONDS 4
static libusb_device_handle *h;
static int done, inflight, ok_pkts, bad_pkts;
static uint32_t phase;
static long frames_left = 48000L * SECONDS;

static void fill(struct libusb_transfer *t) {
  int16_t *s = (int16_t *)t->buffer;
  for (int i = 0; i < PKTS * PKT / 4; i++) {
    int16_t v = 0;
    if (frames_left > 0) {
      v = (int16_t)(sinf(phase * (2 * (float)M_PI / 4294967296.0f)) * 16384);
      phase += (uint32_t)(((uint64_t)440 << 32) / 48000);
      frames_left--;
    }
    s[2 * i] = s[2 * i + 1] = v;
  }
}

static void LIBUSB_CALL cb(struct libusb_transfer *t) {
  for (int i = 0; i < t->num_iso_packets; i++)
    if (t->iso_packet_desc[i].status == LIBUSB_TRANSFER_COMPLETED) ok_pkts++;
    else bad_pkts++;
  if (t->status != LIBUSB_TRANSFER_COMPLETED) fprintf(stderr, "transfer status %d\n", t->status);
  if (frames_left > 0 && t->status == LIBUSB_TRANSFER_COMPLETED) {
    fill(t);
    if (libusb_submit_transfer(t) == 0) return;
  }
  if (--inflight == 0) done = 1;
}

static int ctl(const char *what, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, uint8_t *d, uint16_t n) {
  int r = libusb_control_transfer(h, rt, req, val, idx, d, n, 1000);
  printf("  %-30s %s\n", what, r >= 0 ? "ok" : libusb_error_name(r));
  return r;
}

static void controls(void) {
  uint8_t b[1] = {0};
  int16_t vmax = 0;
  if (ctl("GET_MAX volume ch1", 0xA1, 0x83, 0x0201, 0x0200, (uint8_t *)&vmax, 2) < 0) vmax = 0;
  printf("  (max volume %.2f dB)\n", vmax / 256.0);
  ctl("SET_CUR mute off (master)", 0x21, 0x01, 0x0100, 0x0200, b, 1);
  for (int ch = 1; ch <= 2; ch++) {
    char w[32];
    snprintf(w, sizeof w, "SET_CUR volume max ch%d", ch);
    ctl(w, 0x21, 0x01, 0x0200 | ch, 0x0200, (uint8_t *)&vmax, 2);
  }
}

static void alt1_and_freq(void) {
  int r = libusb_set_interface_alt_setting(h, 1, 1);
  printf("  %-30s %s\n", "SET_INTERFACE 1 alt 1", r == 0 ? "ok" : libusb_error_name(r));
  uint8_t f[3] = {0x80, 0xBB, 0x00};
  ctl("SET_CUR sampling freq 48000", 0x22, 0x01, 0x0100, 0x0003, f, 3);
}

int main(int argc, char **argv) {
  const char *mode = argc > 1 ? argv[1] : "esp";
  unsigned vid = 0, pid = 0;
  if (argc > 2 && sscanf(argv[2], "%x:%x", &vid, &pid) != 2) {
    fprintf(stderr, "device must be VID:PID in hex\n");
    return 1;
  }
  libusb_init(NULL);
  libusb_device **list;
  ssize_t n = libusb_get_device_list(NULL, &list);
  libusb_device *dev = NULL;
  for (ssize_t i = 0; i < n && !dev; i++) {
    struct libusb_device_descriptor dd;
    libusb_get_device_descriptor(list[i], &dd);
    if (vid ? (dd.idVendor == vid && dd.idProduct == pid)
            : ((dd.idVendor == 0x4c4a && dd.idProduct == 0x4155) || (dd.idVendor == 0x5a47 && dd.idProduct == 0x5858)))
      dev = list[i];
  }
  if (!dev) {
    fprintf(stderr, "Speaker not found\n");
    return 1;
  }
  int r = libusb_open(dev, &h);
  if (r) {
    fprintf(stderr, "open: %s\n", libusb_error_name(r));
    return 1;
  }
  libusb_set_auto_detach_kernel_driver(h, 1);
  if (!strcmp(mode, "reset-esp")) {
    r = libusb_reset_device(h);
    printf("USB reset: %s\n", r == 0 ? "ok" : libusb_error_name(r));
    mode = "esp";
  }
  for (int i = 0; i <= 1; i++) {
    r = libusb_claim_interface(h, i);
    if (r) {
      fprintf(stderr, "claim interface %d: %s (run with sudo)\n", i, libusb_error_name(r));
      return 1;
    }
  }
  printf("Mode %s:\n", mode);
  libusb_set_interface_alt_setting(h, 1, 0);  // start idle, like a fresh attach
  if (!strcmp(mode, "esp")) {
    alt1_and_freq();
    controls();
  } else if (!strcmp(mode, "mac")) {
    controls();
    alt1_and_freq();
  } else {
    r = libusb_set_interface_alt_setting(h, 1, 1);
    printf("  %-30s %s\n", "SET_INTERFACE 1 alt 1", r ? libusb_error_name(r) : "ok");
  }

  printf("Playing a 440 Hz tone for %d s...\n", SECONDS);
  for (int i = 0; i < XFERS; i++) {
    struct libusb_transfer *t = libusb_alloc_transfer(PKTS);
    uint8_t *buf = malloc(PKTS * PKT);
    libusb_fill_iso_transfer(t, h, 0x03, buf, PKTS * PKT, PKTS, cb, NULL, 1000);
    libusb_set_iso_packet_lengths(t, PKT);
    fill(t);
    if (libusb_submit_transfer(t) == 0) inflight++;
    else fprintf(stderr, "submit failed\n");
  }
  while (!done) libusb_handle_events(NULL);
  printf("Done: %d packets sent, %d failed\n", ok_pkts, bad_pkts);
  libusb_set_interface_alt_setting(h, 1, 0);
  libusb_release_interface(h, 1);
  libusb_release_interface(h, 0);
  libusb_close(h);
  libusb_free_device_list(list, 1);
  libusb_exit(NULL);
  return 0;
}
