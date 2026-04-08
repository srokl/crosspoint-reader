#include "XtgXthViewerActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Xtc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Epub/converters/DirectPixelWriter.h"
#include "components/UITheme.h"
#include "fontIds.h"

XtgXthViewerActivity::XtgXthViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path)
    : Activity("XtgXthViewer", renderer, mappedInput), filePath(std::move(path)) {}

void XtgXthViewerActivity::onEnter() {
  Activity::onEnter();

  const int screenWidth = renderer.getScreenWidth();
  const int screenHeight = renderer.getScreenHeight();
  Rect popupRect = GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  GUI.fillPopupProgress(renderer, popupRect, 20);

  FsFile file;
  if (!Storage.openFileForRead("XTG", filePath, file)) {
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, "Could not open file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  xtc::XtgPageHeader header;
  if (file.read(&header, sizeof(header)) != sizeof(header)) {
    LOG_ERR("XTG", "Header read failed: %s", filePath.c_str());
    file.close();
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, "Invalid XTG/XTH file");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  // Check magic
  uint8_t bitDepth = 0;
  if (header.magic == xtc::XTG_MAGIC) {
    bitDepth = 1;
  } else if (header.magic == xtc::XTH_MAGIC) {
    bitDepth = 2;
  } else {
    LOG_ERR("XTG", "Invalid magic: 0x%08X", header.magic);
    file.close();
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, screenHeight / 2, "Unsupported format");
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    return;
  }

  if (abs(header.width - screenWidth) > 1 || abs(header.height - screenHeight) > 1) {
    LOG_DBG("XTG", "Size %dx%d does not match screen %dx%d", header.width, header.height, screenWidth, screenHeight);
    // Continue anyway, it might be a small image
  }

  const uint32_t dataOffset = file.position();
  GUI.fillPopupProgress(renderer, popupRect, 50);

  struct XtgCtx {
    FsFile* file;
    uint32_t dataOffset;
    xtc::XtgPageHeader header;
    uint8_t bitDepth;
    MappedInputManager::Labels labels;
  };
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  XtgCtx ctx{&file, dataOffset, header, bitDepth, labels};

  renderer.clearScreen();
  
  if (bitDepth == 1) {
    // 1-bit rendering (XTG): Row-major, 8 pixels per byte, MSB first
    renderer.renderGrayscale(
      GfxRenderer::GrayscaleMode::FactoryFast,
      [](GfxRenderer& r, void* raw) {
        const auto* c = static_cast<const XtgCtx*>(raw);
        c->file->seek(c->dataOffset);

        const int bytesPerRow = (c->header.width + 7) / 8;
        uint8_t* rowBuf = static_cast<uint8_t*>(malloc(bytesPerRow));
        if (!rowBuf) return;

        DirectPixelWriter pw;
        pw.init(r);

        for (int row = 0; row < c->header.height; row++) {
          if (c->file->read(rowBuf, bytesPerRow) != bytesPerRow) break;
          pw.beginRow(row);
          for (int col = 0; col < c->header.width; col++) {
            const bool pixel = (rowBuf[col >> 3] >> (7 - (col & 7))) & 1;
            pw.writePixel(col, pixel ? 3 : 0); // 0=Black, 3=White. bit=1 is white in XTC
          }
        }
        free(rowBuf);
        GUI.drawButtonHints(r, c->labels.btn1, c->labels.btn2, c->labels.btn3, c->labels.btn4);
      },
      &ctx);
  } else {
    // 2-bit rendering (XTH): Bit-Plane Column-Major
    // - Two bit planes
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte
    renderer.renderGrayscale(
      GfxRenderer::GrayscaleMode::FactoryQuality,
      [](GfxRenderer& r, void* raw) {
        const auto* c = static_cast<const XtgCtx*>(raw);
        
        const size_t planeSize = (static_cast<size_t>(c->header.width) * c->header.height + 7) / 8;
        uint8_t* plane1 = static_cast<uint8_t*>(malloc(planeSize));
        uint8_t* plane2 = static_cast<uint8_t*>(malloc(planeSize));
        
        if (plane1 && plane2) {
          c->file->seek(c->dataOffset);
          c->file->read(plane1, planeSize);
          c->file->read(plane2, planeSize);

          const size_t colBytes = (c->header.height + 7) / 8;

          DirectPixelWriter pw;
          pw.init(r);

          for (int row = 0; row < c->header.height; row++) {
            pw.beginRow(row);
            for (int col = 0; col < c->header.width; col++) {
              // Logic from XtcReaderActivity
              const size_t colIndexInFile = c->header.width - 1 - col;
              const size_t byteInCol = row / 8;
              const size_t bitInByte = 7 - (row % 8);
              const size_t byteOffset = colIndexInFile * colBytes + byteInCol;

              const uint8_t b1 = (plane1[byteOffset] >> bitInByte) & 1;
              const uint8_t b2 = (plane2[byteOffset] >> bitInByte) & 1;
              const uint8_t pv = 3 - ((b2 << 1) | b1); // Swapped and Inverted mapping
  pw.writePixel(col, pv);
            }
          }
        }
        if (plane1) free(plane1);
        if (plane2) free(plane2);
        GUI.drawButtonHints(r, c->labels.btn1, c->labels.btn2, c->labels.btn3, c->labels.btn4);
      },
      &ctx);
  }

  file.close();
}

void XtgXthViewerActivity::onExit() {
  Activity::onExit();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void XtgXthViewerActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
}
