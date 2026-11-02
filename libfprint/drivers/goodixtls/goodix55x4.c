// Goodix Tls driver for libfprint

// Copyright (C) 2021 Alexander Meiler <alex.meiler@protonmail.com>
// Copyright (C) 2021 Matthieu CHARETTE <matthieu.charette@gmail.com>
// Copyright (C) 2021 Alireza S.N. <alireza6677@gmail.com>

// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "fp-device.h"
#include "fp-image-device.h"
#include "fp-image.h"
#include "fpi-assembling.h"
#include "fpi-context.h"
#include "fpi-image-device.h"
#include "fpi-image.h"
#include "fpi-ssm.h"
#include "glibconfig.h"
#include "gusb/gusb-device.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#define FP_COMPONENT "goodixtls55x4"

#include <glib.h>
#include <string.h>

#include "drivers_api.h"
#include "goodix.h"
#include "goodix55x4.h"
#include "goodix_proto.h"

#include <math.h>

// 108 columns x 88 rows. Decoding at any other stride shears every row.
// (goodix-fp-dump names these the other way round: its SENSOR_WIDTH=88 is the
// row count -- see the header its write_pgm() emits.)
#define GOODIX55X4_WIDTH 108
#define GOODIX55X4_HEIGHT 88
#define GOODIX55X4_SCAN_WIDTH 108
#define GOODIX55X4_FRAME_SIZE (GOODIX55X4_WIDTH * GOODIX55X4_HEIGHT)
// For every 4 pixels there are 6 bytes and there are 8 extra start bytes and 5
// extra end
#define GOODIX55X4_RAW_FRAME_SIZE                                              \
  (GOODIX55X4_HEIGHT * GOODIX55X4_SCAN_WIDTH) / 4 * 6
#define GOODIX55X4_CAP_FRAMES 1 // Number of frames we capture per swipe

typedef unsigned short Goodix55X4Pix;

struct _FpiDeviceGoodixTls55X4 {
  FpiDeviceGoodixTls parent;

  guint8 *otp;

  GSList *frames;

  Goodix55X4Pix empty_img[GOODIX55X4_FRAME_SIZE];

  FpImage *pending_img;

  gboolean calibrated;   // empty_img is captured once per activation
  gboolean deactivating; // unwind any in-flight FDT poll loop
};

G_DECLARE_FINAL_TYPE(FpiDeviceGoodixTls55X4, fpi_device_goodixtls55x4, FPI,
                         DEVICE_GOODIXTLS55X4, FpiDeviceGoodixTls);

G_DEFINE_TYPE(FpiDeviceGoodixTls55X4, fpi_device_goodixtls55x4,
              FPI_TYPE_DEVICE_GOODIXTLS);

static void goodix55X4_reset_state(FpiDeviceGoodixTls55X4 *self) {
  g_slist_free_full(self->frames, g_free);
  self->frames = NULL;
  g_clear_pointer(&self->otp, g_free);
  memset(self->empty_img, 0, sizeof(self->empty_img));
  g_clear_object(&self->pending_img);
  self->calibrated = FALSE;
  self->deactivating = FALSE;
}

// ---- ACTIVE SECTION START ----

enum activate_states {
  ACTIVATE_READ_AND_NOP,
  ACTIVATE_ENABLE_CHIP,
  ACTIVATE_NOP,
  ACTIVATE_CHECK_FW_VER,
  ACTIVATE_CHECK_PSK,
  ACTIVATE_RESET,
  ACTIVATE_SET_MCU_IDLE,
  ACTIVATE_SET_MCU_CONFIG,
  ACTIVATE_NUM_STATES,
};

static void check_none(FpDevice *dev, gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_firmware_version(FpDevice *dev, gchar *firmware,
                                   gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device firmware: \"%s\"", firmware);
  g_print("%s\n", firmware);

  if (strcmp(firmware, GOODIX_55X4_FIRMWARE_VERSION))
    fp_warn("Unexpected firmware version: \"%s\" (expected \"%s\"), continuing anyway",
            firmware, GOODIX_55X4_FIRMWARE_VERSION);

  fpi_ssm_next_state(user_data);
}

static void check_reset(FpDevice *dev, gboolean success, guint16 number,
                        gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to reset device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fp_dbg("Device reset number: %d", number);

  if (number != GOODIX_55X4_RESET_NUMBER) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device reset number: %d", number);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}

static void check_preset_psk_read(FpDevice *dev, gboolean success,
                                  guint32 flags, guint8 *psk, guint16 length,
                                  gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (!success) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to read PSK from device");
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  g_autofree gchar *psk_str = data_to_str(psk, length);
  fp_dbg("Device PSK: 0x%s", psk_str);
  fp_dbg("Device PSK flags: 0x%08x", flags);

  if (flags != GOODIX_55X4_PSK_FLAGS) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK flags: 0x%08x", flags);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (length != sizeof(goodix_55x4_psk_0)) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK: 0x%s", psk_str);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  if (memcmp(psk, goodix_55x4_psk_0, sizeof(goodix_55x4_psk_0))) {
    g_set_error(&error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid device PSK: 0x%s", psk_str);
    fpi_ssm_mark_failed(user_data, error);
    return;
  }

  fpi_ssm_next_state(user_data);
}
static void check_idle(FpDevice *dev, gpointer user_data, GError *err) {

  if (err) {
    fpi_ssm_mark_failed(user_data, err);
    return;
  }
  fpi_ssm_next_state(user_data);
}
static void check_sleep_realtek(FpDevice *dev, gboolean success, gpointer user_data, GError *err) {

  if (err) {
    fpi_ssm_mark_failed(user_data, err);
    return;
  }
  if (!success) {
    fpi_ssm_mark_failed(user_data,
                        g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                    "failed to put into sleep mode (realtek)"));
    return;
  } else {
    g_print("Device is now on sleep\n");
  }
  fpi_ssm_next_state(user_data);
}
static void check_config_upload(FpDevice *dev, gboolean success,
                                gpointer user_data, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(user_data, error);
  } else if (!success) {
    fpi_ssm_mark_failed(user_data,
                        g_error_new(FP_DEVICE_ERROR, FP_DEVICE_ERROR_PROTO,
                                    "failed to upload mcu config"));
  } else {
    fpi_ssm_next_state(user_data);
  }
}

static void activate_run_state(FpiSsm *ssm, FpDevice *dev) {

  switch (fpi_ssm_get_cur_state(ssm)) {
  case ACTIVATE_READ_AND_NOP:
    g_print("Read and NO OP\n");
    // Nop seems to clear the previous command buffer. But we are
    // unable to do so.
    goodix_start_read_loop(dev);
    goodix_send_nop(dev, check_none, ssm);
    break;

  case ACTIVATE_ENABLE_CHIP:
    g_print("Enable Chip\n");
    goodix_send_enable_chip(dev, TRUE, check_none, ssm);
    break;

  case ACTIVATE_NOP:
    g_print("NO OP\n");
    goodix_send_nop(dev, check_none, ssm);
    break;

  case ACTIVATE_CHECK_FW_VER:
    g_print("Checking FW\n");
    goodix_send_firmware_version(dev, check_firmware_version, ssm);
    break;

  case ACTIVATE_CHECK_PSK:
    g_print("Checking PSK\n");
    goodix_send_preset_psk_read(dev, GOODIX_55X4_PSK_FLAGS, 32,
                                check_preset_psk_read, ssm);
    break;

  case ACTIVATE_RESET:
    g_print("Reset Device\n");
    goodix_send_reset(dev, TRUE, 20, check_reset, ssm);
    break;

  case ACTIVATE_SET_MCU_IDLE:
    g_print("Device IDLE\n");
    goodix_send_mcu_switch_to_idle_mode(dev, 20, check_idle, ssm);
    break;

    // case ACTIVATE_SET_ODP:
    //     goodix_send_read_otp(dev, read_otp_callback, ssm);
    //     break;

  case ACTIVATE_SET_MCU_CONFIG:
    g_print("Uploading Device Config\n");
    goodix_send_upload_config_mcu(dev, goodix_55x4_config,
                                  sizeof(goodix_55x4_config), NULL,
                                  check_config_upload, ssm);
    break;

    // case ACTIVATE_SET_POWERDOWN_SCAN_FREQUENCY:
    //     g_print("Powerdown Scan Freq\n");
    //     /*goodix_send_set_powerdown_scan_frequency(
    //         dev, 100, check_powerdown_scan_freq, ssm);*/
    //     //goodix_send_drv_state(dev, check_powerdown_scan_freq, ssm);
    //     fpi_ssm_next_state(ssm);
    //     break;
  }
}

static void tls_activation_complete(FpDevice *dev, gpointer user_data,
                                    GError *error) {
  if (error) {
    fp_err("failed to complete tls activation: %s", error->message);
    return;
  }
  FpImageDevice *image_dev = FP_IMAGE_DEVICE(dev);

  fpi_image_device_activate_complete(image_dev, error);
}

static void activate_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  G_DEBUG_HERE();
  if (!error)
    goodix_tls(dev, tls_activation_complete, NULL);
  else {
    fp_err("failed during activation: %s (code: %d)", error->message,
           error->code);
    fpi_image_device_activate_complete(FP_IMAGE_DEVICE(dev), error);
  }
}

// ---- ACTIVE SECTION END ----

// -----------------------------------------------------------------------------

// ---- SCAN SECTION START ----

// Follows the capture sequence in goodix-fp-dump/driver_55x4.py: two
// throwaway/background exposures, then arm FDT down and grab the finger.
enum SCAN_STAGES {
  SCAN_STAGE_SWITCH_TO_FDT_MODE,
  SCAN_STAGE_WAIT_FINGER_OFF,
  SCAN_STAGE_DISCARD_FIRST_IMG,
  SCAN_STAGE_SWITCH_TO_FDT_MODE2,
  SCAN_STAGE_IDLE,
  SCAN_STAGE_READ_SENSOR_REG,
  SCAN_STAGE_CALIBRATE,
  SCAN_STAGE_SWITCH_TO_FDT_MODE3,
  SCAN_STAGE_SWITCH_TO_SLEEP,
  SCAN_STAGE_SWITCH_TO_FDT_DOWN,
  SCAN_STAGE_GET_IMG,
  SCAN_STAGE_SWITCH_TO_FDT_MODE4,
  SCAN_STAGE_SWITCH_TO_FDT_UP_NO_REPLY,
  SCAN_STAGE_SWITCH_TO_FDT_UP,
  SCAN_STAGE_SWITCH_TO_FDT_DONE,
  SCAN_STAGE_NUM,
};

enum SLEEP_STAGES {
  SLEEP_STAGE_SWITCH_TO_SLEEP_MODE,
  SLEEP_STAGE_SWITCH_TO_SLEEP_MODE_REALTEK,
  SLEEP_STAGE_DEACTIVATE,
  SLEEP_STAGE_NUM,
};


static void check_none_cmd(FpDevice *dev, guint8 *data, guint16 len,
                           gpointer ssm, GError *err) {
  if (err) {
    g_print("CHECK NONE FAILED\n");
    fpi_ssm_mark_failed(ssm, err);
    return;
  }
  g_print("CHECK NONE SUCCESS\n");
  fpi_ssm_next_state(ssm);
}

static unsigned char get_pix(struct fpi_frame_asmbl_ctx *ctx,
                             struct fpi_frame *frame, unsigned int x,
                             unsigned int y) {
  return frame->data[x + y * GOODIX55X4_WIDTH];
}

static void decode_frame(Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE],
                         const guint8 *raw_frame) {

  Goodix55X4Pix uncropped[GOODIX55X4_SCAN_WIDTH * GOODIX55X4_HEIGHT];
  Goodix55X4Pix *pix = uncropped;
  for (int i = 0; i < GOODIX55X4_RAW_FRAME_SIZE; i += 6) {
    const guint8 *chunk = raw_frame + i;
    *pix++ = ((chunk[0] & 0xf) << 8) + chunk[1];
    *pix++ = (chunk[3] << 4) + (chunk[0] >> 4);
    *pix++ = ((chunk[5] & 0xf) << 8) + chunk[2];
    *pix++ = (chunk[4] << 4) + (chunk[5] >> 4);
  }

  for (int y = 0; y != GOODIX55X4_HEIGHT; ++y) {
    for (int x = 0; x != GOODIX55X4_WIDTH; ++x) {
      const int idx = x + y * GOODIX55X4_SCAN_WIDTH;
      frame[x + y * GOODIX55X4_WIDTH] = uncropped[idx];
    }
  }
}
/**
 * @brief Squashes the 12 bit pixels of a raw frame into the 4 bit pixels used
 * by libfprint.
 * @details Borrowed from the elan driver. We reduce frames to
 * within the max and min.
 *
 * @param frame
 * @param squashed
 */
static void squash_frame_linear(Goodix55X4Pix *frame, guint8 *squashed) {
  Goodix55X4Pix min = 0xffff;
  Goodix55X4Pix max = 0;

  for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i) {
    const Goodix55X4Pix pix = frame[i];
    if (pix < min) {
      min = pix;
    }
    if (pix > max) {
      max = pix;
    }
  }

  for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i) {
    const Goodix55X4Pix pix = frame[i];
    if (pix - min == 0 || max - min == 0) {
      squashed[i] = 0;
    } else {
      squashed[i] = (pix - min) * 0xff / (max - min);
    }
  }
}

/**
 * @brief Subtracts the background from the frame
 *
 * @param frame
 * @param background
 */
static gboolean
postprocess_frame(Goodix55X4Pix frame[GOODIX55X4_FRAME_SIZE],
                  Goodix55X4Pix background[GOODIX55X4_FRAME_SIZE]) {
  int sum = 0;
  for (int i = 0; i != GOODIX55X4_FRAME_SIZE; ++i) {
    Goodix55X4Pix *og_px = frame + i;
    Goodix55X4Pix bg_px = background[i];
    if (bg_px > *og_px) {
      *og_px = bg_px - *og_px;
    } else {
      *og_px -= bg_px;
    }

    sum += *og_px;
  }
  if (sum == 0) {
    fp_warn("frame darker than background, finger on scanner during "
            "calibration?");
  }
  return sum != 0;
}

typedef struct _frame_processing_info {
  FpiDeviceGoodixTls55X4 *dev;
  GSList **frames;

} frame_processing_info;

static void process_frame(Goodix55X4Pix *raw_frame,
                          frame_processing_info *info) {
  struct fpi_frame *frame =
      g_malloc(GOODIX55X4_FRAME_SIZE + sizeof(struct fpi_frame));
  postprocess_frame(raw_frame, info->dev->empty_img);
  squash_frame_linear(raw_frame, frame->data);

  *(info->frames) = g_slist_append(*(info->frames), frame);
}

static void save_frame(FpiDeviceGoodixTls55X4 *self, guint8 *raw) {
  Goodix55X4Pix *frame = malloc(GOODIX55X4_FRAME_SIZE * sizeof(Goodix55X4Pix));
  decode_frame(frame, raw);
  self->frames = g_slist_append(self->frames, frame);
}

static void scan_on_read_img(FpDevice *dev, guint8 *data, guint16 len,
                             gpointer ssm, GError *err) {
  g_print("SCAN_ON_READ_IMG\n");
  if (err) {
    fpi_ssm_mark_failed(ssm, err);
    return;
  }

  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(dev);
  save_frame(self, data);
  if (g_slist_length(self->frames) < GOODIX55X4_CAP_FRAMES) {
    // Re-arm FDT down only -- jumping back to the top would re-run
    // SCAN_STAGE_CALIBRATE with the finger still down.
    fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_DOWN);
  } else {
    GSList *raw_frames = self->frames;

    struct fpi_frame_asmbl_ctx assembly_ctx;
    assembly_ctx.frame_width = GOODIX55X4_WIDTH;
    assembly_ctx.frame_height = GOODIX55X4_HEIGHT;
    assembly_ctx.image_width = GOODIX55X4_WIDTH * 1;
    assembly_ctx.get_pixel = get_pix;

    GSList *frames = NULL;
    frame_processing_info pinfo = {.dev = self, .frames = &frames};

    g_slist_foreach(raw_frames, (GFunc)process_frame, &pinfo);

    g_print("MOVEMENT EST\n");
    fpi_do_movement_estimation(&assembly_ctx, frames);
    g_print("MOVEMENT EST DOOONEE\n");
    FpImage *img = fpi_assemble_frames(&assembly_ctx, frames);

    g_slist_free_full(frames, g_free);
    g_slist_free_full(self->frames, g_free);
    self->frames = NULL;

    g_print("Next State\n");
    self->pending_img = img;
    fpi_ssm_next_state(ssm);
  }
}

gboolean save_image_to_pgm2(guchar *data, const char *path) {
  FILE *fd = fopen(path, "w");
  size_t write_size = GOODIX55X4_FRAME_SIZE;
  int r;

  if (!fd) {
    g_warning("could not open '%s' for writing: %d", path, errno);
    return FALSE;
  }

  r = fprintf(fd, "P2\n%d %d\n255\n", GOODIX55X4_WIDTH, GOODIX55X4_HEIGHT);
  if (r < 0) {
    fclose(fd);
    g_critical("pgm header write failed, error %d", r);
    return FALSE;
  }

  for (int i = 0; i < write_size; i += 1) {
    r = fprintf(fd, "%d\n", data[i]);
  }
  fclose(fd);
  g_debug("written to '%s'", path);

  return TRUE;
}

gboolean save_image_to_pgm(FpImage *img, const char *path) {
  FILE *fd = fopen(path, "w");
  size_t write_size;
  const guchar *data = fp_image_get_data(img, &write_size);
  int r;

  if (!fd) {
    g_warning("could not open '%s' for writing: %d", path, errno);
    return FALSE;
  }

  r = fprintf(fd, "P5 %d %d 255\n", fp_image_get_width(img),
              fp_image_get_height(img));
  if (r < 0) {
    fclose(fd);
    g_critical("pgm header write failed, error %d", r);
    return FALSE;
  }

  r = fwrite(data, 1, write_size, fd);
  if (r < write_size) {
    fclose(fd);
    g_critical("short write (%d)", r);
    return FALSE;
  }

  fclose(fd);
  g_debug("written to '%s'", path);

  return TRUE;
}

enum scan_empty_img_state {
  SCAN_EMPTY_GET_IMG,

  SCAN_EMPTY_NUM,
};

static void on_scan_empty_img(FpDevice *dev, guint8 *data, guint16 length,
                              gpointer ssm, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(ssm, error);
    return;
  }
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(dev);
  decode_frame(self->empty_img, data);
  self->calibrated = TRUE;
  fpi_ssm_next_state(ssm);
}
static void scan_empty_run(FpiSsm *ssm, FpDevice *dev) {

  switch (fpi_ssm_get_cur_state(ssm)) {

  case SCAN_EMPTY_GET_IMG:
    goodix_tls_read_image(dev, on_scan_empty_img, ssm);
    break;
  }
}

// FDT down/up do not block until the finger moves -- they answer immediately
// with the sensor's current state, so we have to poll. Reply layout, measured
// on 27c6:55a4 (GF3268_RTSEC_APP_10052):
//
//   [0]     0x80 no finger, 0x02 finger present
//   [2..3]  LE 12-bit per-channel contact mask, 0x000 .. 0xfff
//   [4..]   twelve LE16 channel readings (~350 idle, ~250 under a finger)
//
// The USB round trip (~40ms) paces the loop.
static guint16 fdt_touch_mask(const guint8 *data, guint16 len) {
  return (len >= 4) ? data[2] | (data[3] << 8) : 0;
}

// The FDT polls are unbounded -- they re-arm until the finger moves. If
// deactivation lands mid-wait, stop instead of looping against a device
// that dev_deactivate() is already putting to sleep.
static gboolean scan_cancelled(FpDevice *dev, FpiSsm *ssm) {
  if (!FPI_DEVICE_GOODIXTLS55X4(dev)->deactivating)
    return FALSE;
  fpi_ssm_mark_completed(ssm);
  return TRUE;
}

static void check_fdt_down(FpDevice *dev, guint8 *data, guint16 len,
                           gpointer ssm, GError *err) {
  if (err) {
    fpi_ssm_mark_failed(ssm, err);
    return;
  }

  const guint16 touch_mask = fdt_touch_mask(data, len);

  if (touch_mask == 0) {
    if (scan_cancelled(dev, ssm))
      return;
    fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_DOWN);
    return;
  }

  fp_dbg("finger down, contact mask 0x%03x", touch_mask);
  fpi_ssm_next_state(ssm);
}

// Calibration must run on a bare sensor: empty_img is subtracted from every
// capture, so a finger resting here cancels out the ridges.
static void check_wait_finger_off(FpDevice *dev, guint8 *data, guint16 len,
                                  gpointer ssm, GError *err) {
  if (err) {
    fpi_ssm_mark_failed(ssm, err);
    return;
  }

  const guint16 touch_mask = fdt_touch_mask(data, len);

  if (touch_mask != 0) {
    if (scan_cancelled(dev, ssm))
      return;
    fpi_ssm_jump_to_state(ssm, SCAN_STAGE_WAIT_FINGER_OFF);
    return;
  }

  fp_dbg("sensor clear, calibrating");
  fpi_ssm_next_state(ssm);
}

// Opposite condition: without this a single press satisfies several enroll
// stages in a row.
static void check_fdt_up(FpDevice *dev, guint8 *data, guint16 len,
                         gpointer ssm, GError *err) {
  if (err) {
    fpi_ssm_mark_failed(ssm, err);
    return;
  }

  const guint16 touch_mask = fdt_touch_mask(data, len);

  if (touch_mask != 0) {
    if (scan_cancelled(dev, ssm))
      return;
    fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_UP);
    return;
  }

  fp_dbg("finger lifted");
  fpi_ssm_next_state(ssm);
}

// First get_image after config upload returns a stale exposure; discard it.
static void on_discard_img(FpDevice *dev, guint8 *data, guint16 length,
                           gpointer ssm, GError *error) {
  if (error) {
    fpi_ssm_mark_failed(ssm, error);
    return;
  }
  fpi_ssm_next_state(ssm);
}

static void scan_empty_img(FpDevice *dev, FpiSsm *ssm) {
  fpi_ssm_start_subsm(ssm, fpi_ssm_new(dev, scan_empty_run, SCAN_EMPTY_NUM));
}

static void scan_get_img(FpDevice *dev, FpiSsm *ssm) {
  goodix_tls_read_image(dev, scan_on_read_img, ssm);
}

/* FDT threshold bytes verified from USB capture with 55a4 device (goodix-fp-dump) */
const guint8 fdt_switch_state_mode_55X4[] = {
    0x0d, 0x01, 0x80, 0x12, 0x80, 0x12, 0x80, 0x98, 0x80,
    0x82, 0x80, 0x12, 0x80, 0xa0, 0x80, 0x99, 0x80, 0x7f,
    0x80, 0x12, 0x80, 0x9f, 0x80, 0x93, 0x80, 0x7e};

/* mode2 reuses mode1 thresholds (same as Python tool second pass) */
const guint8 fdt_switch_state_mode2_55X4[] = {
    0x0d, 0x01, 0x80, 0x12, 0x80, 0x12, 0x80, 0x98, 0x80,
    0x82, 0x80, 0x12, 0x80, 0xa0, 0x80, 0x99, 0x80, 0x7f,
    0x80, 0x12, 0x80, 0x9f, 0x80, 0x93, 0x80, 0x7e};

/* FDT down bytes verified from USB capture with 55a4 device */
const guint8 fdt_switch_state_down_55X4[] = {
    0x0c, 0x01, 0x80, 0xb0, 0x80, 0xc4, 0x80, 0xba, 0x80,
    0xa6, 0x80, 0xb7, 0x80, 0xc7, 0x80, 0xc0, 0x80, 0xaa,
    0x80, 0xb4, 0x80, 0xc4, 0x80, 0xba, 0x80, 0xa6};

const guint8 fdt_switch_state_up_55X4[] = {
    0x0e, 0x01, 0x80, 0x92, 0x80, 0x9d, 0x80, 0x93, 0x80,
    0x92, 0x80, 0x97, 0x80, 0x9e, 0x80, 0xa0, 0x80, 0x8e,
    0x80, 0xab, 0x80, 0xa5, 0x80, 0xb0, 0x80, 0x12};

static void scan_run_state(FpiSsm *ssm, FpDevice *dev) {
  FpImageDevice *img_dev = FP_IMAGE_DEVICE(dev);
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(dev);

  switch (fpi_ssm_get_cur_state(ssm)) {
  case SCAN_STAGE_SWITCH_TO_FDT_MODE:
    g_print("SWITCH TO FDT MODE\n");
    goodix_send_mcu_switch_to_fdt_mode(
        dev, (guint8 *)fdt_switch_state_mode_55X4,
        sizeof(fdt_switch_state_mode_55X4), NULL, check_none_cmd, ssm);
    break;

  case SCAN_STAGE_WAIT_FINGER_OFF:
    // Calibrate once per activation. Enrollment runs ~10 scans back to back
    // with the finger returning within a second, so re-calibrating per scan
    // just captures the finger as the background.
    if (self->calibrated) {
      fpi_ssm_jump_to_state(ssm, SCAN_STAGE_SWITCH_TO_FDT_MODE3);
      break;
    }
    // Polled; no g_print, it re-runs until the sensor is clear.
    goodix_send_mcu_switch_to_fdt_up(dev, (guint8 *)fdt_switch_state_up_55X4,
                                     sizeof(fdt_switch_state_up_55X4), NULL,
                                     check_wait_finger_off, ssm);
    break;

  case SCAN_STAGE_DISCARD_FIRST_IMG:
    g_print("DISCARD FIRST IMAGE\n");
    goodix_tls_read_image(dev, on_discard_img, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_FDT_MODE2:
    g_print("SWITCH TO FDT MODE 2\n");
    goodix_send_mcu_switch_to_fdt_mode(
        dev, (guint8 *)fdt_switch_state_mode_55X4,
        sizeof(fdt_switch_state_mode_55X4), NULL, check_none_cmd, ssm);
    break;

  case SCAN_STAGE_IDLE:
    g_print("SWITCH TO IDLE\n");
    goodix_send_mcu_switch_to_idle_mode(dev, 20, check_idle, ssm);
    break;

  case SCAN_STAGE_READ_SENSOR_REG:
    g_print("READ SENSOR REGISTER\n");
    goodix_send_read_sensor_register(dev, 0x0082, 2, check_none_cmd, ssm);
    break;

  case SCAN_STAGE_CALIBRATE:
    scan_empty_img(dev, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_FDT_MODE3:
    g_print("SWITCH TO FDT MODE 3\n");
    goodix_send_mcu_switch_to_fdt_mode(
        dev, (guint8 *)fdt_switch_state_mode_55X4,
        sizeof(fdt_switch_state_mode_55X4), NULL, check_none_cmd, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_SLEEP:
    g_print("SWITCH TO SLEEP\n");
    goodix_send_mcu_switch_to_sleep_mode_realtek(dev, 0x6c, check_sleep_realtek, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_FDT_DOWN:
    // Polled: no g_print here, it re-runs every ~40ms until contact.
    goodix_send_mcu_switch_to_fdt_down(
        dev, (guint8 *)fdt_switch_state_down_55X4,
        sizeof(fdt_switch_state_down_55X4), NULL, check_fdt_down, ssm);
    break;

  case SCAN_STAGE_GET_IMG:
    g_print("SWITCH TO GET IMAGE\n");
    fpi_image_device_report_finger_status(img_dev, TRUE);
    scan_get_img(dev, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_FDT_MODE4:
    g_print("SWITCH TO FDT MODE 4\n");
    goodix_send_mcu_switch_to_fdt_mode(
        dev, (guint8 *)fdt_switch_state_mode2_55X4,
        sizeof(fdt_switch_state_mode2_55X4), NULL, check_none_cmd, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_FDT_UP_NO_REPLY:
    g_print("SWITCH TO FDT UP NO REPLY\n");
    goodix_send_mcu_switch_to_fdt_up_no_reply(
        dev, (guint8 *)fdt_switch_state_up_55X4,
        sizeof(fdt_switch_state_up_55X4), NULL, check_none_cmd, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_FDT_UP:
    // Polled until the finger comes off; no g_print for the same reason.
    goodix_send_mcu_switch_to_fdt_up(dev, (guint8 *)fdt_switch_state_up_55X4,
                                     sizeof(fdt_switch_state_up_55X4), NULL,
                                     check_fdt_up, ssm);
    break;

  case SCAN_STAGE_SWITCH_TO_FDT_DONE: {
    if (self->pending_img) {
      g_print("Signal IMG Capture\n");
      fpi_image_device_image_captured(img_dev, self->pending_img);
      self->pending_img = NULL;
    }
    fpi_image_device_report_finger_status(img_dev, FALSE);
    fpi_ssm_mark_completed(ssm);
    break;
  }
  }
}

static void sleep_run_state(FpiSsm *ssm, FpDevice *dev) {
  FpImageDevice *img_dev = FP_IMAGE_DEVICE(dev);

  switch (fpi_ssm_get_cur_state(ssm)) {
  case SLEEP_STAGE_SWITCH_TO_SLEEP_MODE:
    goodix_reset_state(dev);
    g_print("SWITCH TO SLEEP MODE\n");
    goodix_send_mcu_switch_to_sleep_mode(dev, 20, check_idle, ssm);

    break;
  case SLEEP_STAGE_SWITCH_TO_SLEEP_MODE_REALTEK:
    g_print("SWITCH TO SLEEP MODE REALTEK\n");
    goodix_send_mcu_switch_to_sleep_mode_realtek(dev, 0x6c, check_sleep_realtek, ssm);
    break;
  case SLEEP_STAGE_DEACTIVATE:
    g_print("Deactivated\n");
    goodix_reset_state(dev);
//    goodix_cancel_receive(dev);
    GError *error = NULL;
    goodix_shutdown_tls(dev, &error);
    goodix55X4_reset_state(FPI_DEVICE_GOODIXTLS55X4(img_dev));
    fpi_image_device_deactivate_complete(img_dev, error);
    break;
  }  
}

static void scan_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  if (error) {
    fp_err("failed to scan: %s (code: %d)", error->message, error->code);
    fpi_image_device_session_error(FP_IMAGE_DEVICE(dev), error);
    return;
  }
  g_print("finished scan!");
  fp_dbg("finished scan");
}

static void sleep_complete(FpiSsm *ssm, FpDevice *dev, GError *error) {
  if (error) {
    fp_err("failed to sleep: %s (code: %d)", error->message, error->code);
    return;
  }
  g_print("finished sleep!");
  fp_dbg("finished sleep");
}

static void scan_start(FpiDeviceGoodixTls55X4 *dev) {
  fpi_ssm_start(fpi_ssm_new(FP_DEVICE(dev), scan_run_state, SCAN_STAGE_NUM),
                scan_complete);
}

static void sleep_start(FpDevice *dev, gpointer user_data) {
  fpi_ssm_start(fpi_ssm_new(dev, sleep_run_state, SLEEP_STAGE_NUM),
                sleep_complete);
}

// ---- SCAN SECTION END ----

// ---- DEV SECTION START ----

static void dev_init(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  goodix_dev_init(dev, &error);
  fpi_image_device_open_complete(img_dev, error);
}

static void dev_deinit(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  GError *error = NULL;

  if (goodix_dev_deinit(dev, &error)) {
    fpi_image_device_close_complete(img_dev, error);
    return;
  }

  fpi_image_device_close_complete(img_dev, NULL);
}

static void dev_activate(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);

  fpi_ssm_start(fpi_ssm_new(dev, activate_run_state, ACTIVATE_NUM_STATES),
                activate_complete);
}

static void dev_change_state(FpImageDevice *img_dev,
                             FpiImageDeviceState state) {
  FpiDeviceGoodixTls55X4 *self = FPI_DEVICE_GOODIXTLS55X4(img_dev);
  G_DEBUG_HERE();

  if (state == FPI_IMAGE_DEVICE_STATE_AWAIT_FINGER_ON)
    scan_start(self);
}


static void dev_deactivate(FpImageDevice *img_dev) {
  FpDevice *dev = FP_DEVICE(img_dev);
  FPI_DEVICE_GOODIXTLS55X4(img_dev)->deactivating = TRUE;
  fpi_device_add_timeout(dev, 250, sleep_start, NULL, NULL);
}

// ---- DEV SECTION END ----

static void fpi_device_goodixtls55x4_init(FpiDeviceGoodixTls55X4 *self) {
  self->frames = NULL;
}

static void
fpi_device_goodixtls55x4_class_init(FpiDeviceGoodixTls55X4Class *class) {
  FpiDeviceGoodixTlsClass *gx_class = FPI_DEVICE_GOODIXTLS_CLASS(class);
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(class);
  FpImageDeviceClass *img_dev_class = FP_IMAGE_DEVICE_CLASS(class);

  gx_class->interface = GOODIX_55X4_INTERFACE;
  gx_class->ep_in = GOODIX_55X4_EP_IN;
  gx_class->ep_out = GOODIX_55X4_EP_OUT;

  dev_class->id = "goodixtls55x4";
  dev_class->full_name = "Goodix TLS Fingerprint Sensor 55X4";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = 10;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  img_dev_class->bz3_threshold = 40;
  img_dev_class->algorithm = FPI_DEVICE_ALGO_SIGFM;
  img_dev_class->img_width = GOODIX55X4_WIDTH;
  img_dev_class->img_height = GOODIX55X4_HEIGHT;

  img_dev_class->img_open = dev_init;
  img_dev_class->img_close = dev_deinit;
  img_dev_class->activate = dev_activate;
  img_dev_class->change_state = dev_change_state;
  img_dev_class->deactivate = dev_deactivate;

  fpi_device_class_auto_initialize_features(dev_class);
}
