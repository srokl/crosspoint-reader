#include "SleepActivity.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "Epub/converters/DirectPixelWriter.h"

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/converters/DirectPixelWriter.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/Logo120.h"

void SleepActivity::onEnter() {
  Activity::onEnter();
  GUI.drawPopup(renderer, tr(STR_ENTERING_SLEEP));

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM):
      return renderCustomSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      return renderCoverSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");
  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    if (dir) dir.close();
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid image files
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        file.close();
        continue;
      }

      const bool isBmp = FsHelpers::hasBmpExtension(filename);
      const bool isPxc = FsHelpers::hasPxcExtension(filename);
      const bool isXtg = FsHelpers::hasXtgExtension(filename);
      const bool isXth = FsHelpers::hasXthExtension(filename);

      if (!isBmp && !isPxc && !isXtg && !isXth) {
        LOG_DBG("SLP", "Skipping non-image file: %s", name);
        file.close();
        continue;
      }
      if (isBmp) {
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() != BmpReaderError::Ok) {
          LOG_DBG("SLP", "Skipping invalid BMP file: %s", name);
          file.close();
          continue;
        }
      }
      if (isPxc) {
        uint16_t w, h;
        if (file.read(&w, 2) != 2 || file.read(&h, 2) != 2) {
          LOG_DBG("SLP", "Skipping PXC with unreadable header: %s", name);
          file.close();
          continue;
        }
        const int sw = renderer.getScreenWidth();
        const int sh = renderer.getScreenHeight();
        if (abs(w - sw) > 1 || abs(h - sh) > 1) {
          LOG_DBG("SLP", "Skipping PXC size mismatch %dx%d (screen %dx%d): %s", w, h, sw, sh, name);
          file.close();
          continue;
        }
      }
      files.emplace_back(filename);
      file.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Generate a random number between 1 and numFiles
      auto randomFileIndex = random(numFiles);
      // If we picked the same image as last time, reroll
      while (numFiles > 1 && APP_STATE.lastSleepImage != UINT8_MAX && randomFileIndex == APP_STATE.lastSleepImage) {
        randomFileIndex = random(numFiles);
      }
      APP_STATE.lastSleepImage = randomFileIndex;
      APP_STATE.saveToFile();
      const auto filename = std::string(sleepDir) + "/" + files[randomFileIndex];
      LOG_DBG("SLP", "Randomly loading: %s/%s", sleepDir, files[randomFileIndex].c_str());
      delay(100);
      if (FsHelpers::hasPxcExtension(files[randomFileIndex])) {
        renderPxcSleepScreen(filename);
        dir.close();
        return;
      }
      if (FsHelpers::hasXtgExtension(files[randomFileIndex])) {
        renderXtgSleepScreen(filename);
        dir.close();
        return;
      }
      if (FsHelpers::hasXthExtension(files[randomFileIndex])) {
        renderXthSleepScreen(filename);
        dir.close();
        return;
      }
      FsFile file;
      if (Storage.openFileForRead("SLP", filename, file)) {
        Bitmap bitmap(file, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          file.close();
          dir.close();
          return;
        }
        file.close();
      }
    }
  }
  if (dir) dir.close();

  // Check root for preferred images
  if (Storage.exists("/sleep.pxc")) {
    LOG_DBG("SLP", "Loading: /sleep.pxc");
    renderPxcSleepScreen("/sleep.pxc");
    return;
  }

  if (Storage.exists("/sleep.xtg")) {
    LOG_DBG("SLP", "Loading: /sleep.xtg");
    renderXtgSleepScreen("/sleep.xtg");
    return;
  }

  if (Storage.exists("/sleep.xth")) {
    LOG_DBG("SLP", "Loading: /sleep.xth");
    renderXthSleepScreen("/sleep.xth");
    return;
  }
  FsFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void SleepActivity::renderPxcSleepScreen(const std::string& path) const {
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) {
    LOG_ERR("SLP", "Cannot open PXC: %s", path.c_str());
    return renderDefaultSleepScreen();
  }

  uint16_t pxcWidth, pxcHeight;
  if (file.read(&pxcWidth, 2) != 2 || file.read(&pxcHeight, 2) != 2) {
    LOG_ERR("SLP", "PXC header read failed: %s", path.c_str());
    file.close();
    return renderDefaultSleepScreen();
  }

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  if (abs(pxcWidth - screenWidth) > 1 || abs(pxcHeight - screenHeight) > 1) {
    LOG_ERR("SLP", "PXC size %dx%d does not match screen %dx%d", pxcWidth, pxcHeight, screenWidth, screenHeight);
    file.close();
    return renderDefaultSleepScreen();
  }

  const uint32_t dataOffset = file.position();
  const auto filter = SETTINGS.sleepScreenCoverFilter;
  const int bytesPerRow = (pxcWidth + 3) / 4;

  if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER) {
    struct PxcCtx {
      FsFile* file;
      uint32_t dataOffset;
      int width, height;
    };
    PxcCtx ctx{&file, dataOffset, pxcWidth, pxcHeight};

    renderer.renderGrayscale(
        GfxRenderer::GrayscaleMode::FactoryQuality,
        [](const GfxRenderer& r, const void* raw) {
          const auto* c = static_cast<const PxcCtx*>(raw);
          c->file->seek(c->dataOffset);

          const int bpr = (c->width + 3) / 4;
          uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bpr));
          if (!rowBuf) {
            LOG_ERR("SLP", "malloc failed for rowBuf (%d bytes, %dx%d)", bpr, c->width, c->height);
            return;
          }

          DirectPixelWriter pw;
          pw.init(r);

          for (int row = 0; row < c->height; row++) {
            if (c->file->read(rowBuf, bpr) != bpr) break;
            pw.beginRow(row);
            for (int col = 0; col < c->width; col++) {
              const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
              pw.writePixel(col, pv);
            }
          }
          free(rowBuf);
        },
        &ctx);
  } else {
    // BLACK_AND_WHITE / INVERTED_BLACK_AND_WHITE: threshold PXC to 1-bit
    // (pv 0=Black, 1=DarkGrey map to dark; 2=LightGrey, 3=White map to light)
    renderer.clearScreen();
    if (!file.seek(dataOffset)) {
      LOG_ERR("SLP", "PXC seek failed: %s", path.c_str());
      file.close();
      return renderDefaultSleepScreen();
    }

    uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bytesPerRow));
    if (!rowBuf) {
      LOG_ERR("SLP", "PXC malloc failed");
      file.close();
      return renderDefaultSleepScreen();
    }

    for (int row = 0; row < pxcHeight; row++) {
      if (file.read(rowBuf, bytesPerRow) != bytesPerRow) break;
      for (int col = 0; col < pxcWidth; col++) {
        const uint8_t pv = (rowBuf[col >> 2] >> (6 - (col & 3) * 2)) & 0x03;
        if (pv < 2) renderer.drawPixel(col, row, true);
      }
    }
    free(rowBuf);

    if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }

  file.close();
}

void SleepActivity::renderXtgSleepScreen(const std::string& path) const {
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) {
    return renderDefaultSleepScreen();
  }

  xtc::XtgPageHeader header;
  if (file.read(&header, sizeof(header)) != sizeof(header)) {
    file.close();
    return renderDefaultSleepScreen();
  }

  struct XtgCtx {
    FsFile* file;
    uint32_t dataOffset;
    int width, height;
  };
  XtgCtx ctx{&file, (uint32_t)file.position(), header.width, header.height};

  renderer.clearScreen();
  renderer.renderGrayscale(
      GfxRenderer::GrayscaleMode::FactoryFast,
      [](const GfxRenderer& r, const void* raw) {
        const auto* c = static_cast<const XtgCtx*>(raw);
        c->file->seek(c->dataOffset);

        const int bytesPerRow = (c->width + 7) / 8;
        uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bytesPerRow));
        if (!rowBuf) return;

        DirectPixelWriter pw;
        pw.init(r);

        for (int row = 0; row < c->height; row++) {
          if (c->file->read(rowBuf, bytesPerRow) != bytesPerRow) break;
          pw.beginRow(row);
          for (int col = 0; col < c->width; col++) {
            const bool pixel = (rowBuf[col >> 3] >> (7 - (col & 7))) & 1;
            pw.writePixel(col, pixel ? 3 : 0);
          }
        }
        free(rowBuf);
      },
      &ctx);
  file.close();
}

void SleepActivity::renderXthSleepScreen(const std::string& path) const {
  FsFile file;
  if (!Storage.openFileForRead("SLP", path, file)) {
    return renderDefaultSleepScreen();
  }

  xtc::XtgPageHeader header;
  if (file.read(&header, sizeof(header)) != sizeof(header)) {
    file.close();
    return renderDefaultSleepScreen();
  }

  struct XthCtx {
    FsFile* file;
    uint32_t dataOffset;
    int width, height;
  };
  XthCtx ctx{&file, (uint32_t)file.position(), header.width, header.height};

  const auto xtgGrayFn = [](const GfxRenderer& r, const void* raw) {
    const auto* c = static_cast<const XthCtx*>(raw);
    c->file->seek(c->dataOffset);

    const size_t planeSize = (static_cast<size_t>(c->width) * c->height + 7) / 8;
    uint8_t* plane1 = static_cast<uint8_t*>(malloc(planeSize));
    uint8_t* plane2 = static_cast<uint8_t*>(malloc(planeSize));

    if (plane1 && plane2) {
      c->file->read(plane1, planeSize);
      c->file->read(plane2, planeSize);

      const size_t colStride = (c->height + 7) / 8;
      const size_t initialOffset = (c->width - 1) * colStride;

      DirectPixelWriter pw;
      pw.init(r);

      for (int row = 0; row < c->height; row++) {
        const size_t byteInCol = row >> 3;
        const uint8_t bitInByte = 7 - (row & 7);
        const uint8_t* p1 = plane1 + initialOffset + byteInCol;
        const uint8_t* p2 = plane2 + initialOffset + byteInCol;

        pw.beginRow(row);
        for (int col = 0; col < c->width; col++) {
          const uint8_t b1 = (*p1 >> bitInByte) & 1;
          const uint8_t b2 = (*p2 >> bitInByte) & 1;
          const uint8_t pv = 3 - ((b2 << 1) | b1);
          pw.writePixel(col, pv);
          
          p1 -= colStride;
          p2 -= colStride;
        }
      }
      free(plane1);
      free(plane2);
    }
  };

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAY2_LSB);
  xtgGrayFn(renderer, &ctx);
  renderer.copyGrayscaleLsbBuffers();

  renderer.clearScreen(0x00);
  renderer.setRenderMode(GfxRenderer::GRAY2_MSB);
  xtgGrayFn(renderer, &ctx);
  renderer.copyGrayscaleMsbBuffers();

  extern const unsigned char lut_factory_quality[];
  renderer.displayGrayBuffer(lut_factory_quality, true);
  renderer.setRenderMode(GfxRenderer::BW);

  file.close();
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    if (ratio > screenRatio) {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
    } else {
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
      y = 0;
    }
  } else {
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  LOG_DBG("SLP", "drawing to %d x %d", x, y);

  const bool hasGreyscale = bitmap.hasGreyscale() &&
                            SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  if (hasGreyscale) {
    struct BitmapGrayCtx {
      const Bitmap* bitmap;
      int x, y, maxWidth, maxHeight;
      float cropX, cropY;
    };
    BitmapGrayCtx grayCtx{&bitmap, x, y, pageWidth, pageHeight, cropX, cropY};
    renderer.renderGrayscale(
        GfxRenderer::GrayscaleMode::FactoryQuality,
        [](const GfxRenderer& r, const void* raw) {
          const auto* c = static_cast<const BitmapGrayCtx*>(raw);
          if (c->bitmap->rewindToData() != BmpReaderError::Ok) {
            LOG_ERR("SLP", "rewindToData failed in grayscale pass");
            return;
          }
          r.drawBitmap(*c->bitmap, c->x, c->y, c->maxWidth, c->maxHeight, c->cropX, c->cropY);
        },
        &grayCtx);
  } else {
    renderer.clearScreen();
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    if (SETTINGS.sleepScreenCoverFilter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  void (SleepActivity::*renderNoCoverSleepScreen)() const;
  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      renderNoCoverSleepScreen = &SleepActivity::renderCustomSleepScreen;
      break;
    default:
      renderNoCoverSleepScreen = &SleepActivity::renderDefaultSleepScreen;
      break;
  }

  if (APP_STATE.openEpubPath.empty()) {
    return (this->*renderNoCoverSleepScreen)();
  }

  std::string coverBmpPath;
  bool cropped = SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP;

  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      return (this->*renderNoCoverSleepScreen)();
    }
    if (!lastXtc.generateCoverBmp()) {
      return (this->*renderNoCoverSleepScreen)();
    }
    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath)) {
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      return (this->*renderNoCoverSleepScreen)();
    }
    if (!lastTxt.generateCoverBmp()) {
      return (this->*renderNoCoverSleepScreen)();
    }
    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastEpub.load(true, true)) {
      return (this->*renderNoCoverSleepScreen)();
    }
    if (!lastEpub.generateCoverBmp(cropped)) {
      return (this->*renderNoCoverSleepScreen)();
    }
    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
  } else {
    return (this->*renderNoCoverSleepScreen)();
  }

  FsFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderBitmapSleepScreen(bitmap);
      file.close();
      return;
    }
    file.close();
  }

  return (this->*renderNoCoverSleepScreen)();
}

void SleepActivity::renderBlankSleepScreen() const {
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
