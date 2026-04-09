/**
 * XtcReaderActivity.cpp
 *
 * XTC ebook reader activity implementation
 * Displays pre-rendered XTC pages on e-ink display
 */

#include "XtcReaderActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include <Epub.h>
#include <Epub/converters/DirectPixelWriter.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "MappedInputManager.h"
#include "ReaderUtils.h"
#include "RecentBooksStore.h"
#include "XtcReaderChapterSelectionActivity.h"
#include "components/UITheme.h"
#include "components/themes/lyra/LyraCarouselTheme.h"

#include "fontIds.h"

namespace {
constexpr unsigned long skipChapterMs = 700;
constexpr unsigned long doubleClickWindowMs = 350;
constexpr unsigned long goHomeMs = 1000;
constexpr uint32_t kTenPageSkip = 10;
}  // namespace

void XtcReaderActivity::onEnter() {
  Activity::onEnter();

  if (!xtc) {
    return;
  }

  xtc->setupCacheDir();

  // Load saved progress
  loadProgress();

  // Save current XTC as last opened book and add to recent books
  APP_STATE.openEpubPath = xtc->getPath();
  APP_STATE.saveToFile();
  RECENT_BOOKS.addBook(xtc->getPath(), xtc->getTitle(), xtc->getAuthor(), xtc->getThumbBmpPath());

  // Trigger first update
  requestUpdate();
}

void XtcReaderActivity::onExit() {
  Activity::onExit();

  APP_STATE.readerActivityLoadCount = 0;
  APP_STATE.saveToFile();

  // Generate carousel thumbnails while XTC is still loaded so the home screen
  // can display the cover on the very first render without a loading popup.
  if (xtc &&
      static_cast<CrossPointSettings::UI_THEME>(SETTINGS.uiTheme) == CrossPointSettings::UI_THEME::LYRA_CAROUSEL) {
    xtc->generateThumbBmp(LyraCarouselTheme::kCenterCoverW, LyraCarouselTheme::kCenterCoverH);
    xtc->generateThumbBmp(LyraCarouselTheme::kSideCoverW, LyraCarouselTheme::kSideCoverH);
  }

  xtc.reset();
}

void XtcReaderActivity::loop() {
  // Enter chapter selection activity
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (xtc && xtc->hasChapters() && !xtc->getChapters().empty()) {
      startActivityForResult(
          std::make_unique<XtcReaderChapterSelectionActivity>(renderer, mappedInput, xtc, currentPage),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              currentPage = std::get<PageResult>(result.data).page;
            }
          });
    }
  }

  // Long press BACK (1s+) goes to file selection
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= goHomeMs) {
    activityManager.goToFileBrowser(xtc ? xtc->getPath() : "");
    return;
  }

  // Short press BACK goes directly to home
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) && mappedInput.getHeldTime() < goHomeMs) {
    onGoHome();
    return;
  }

  // Long press: skip to next/previous chapter. No-op if the file has no chapters.
  if (SETTINGS.longPressChapterSkip && xtc->hasChapters()) {
    const bool skipFwd = mappedInput.wasLongPressed(MappedInputManager::Button::PageForward, skipChapterMs) ||
                         mappedInput.wasLongPressed(MappedInputManager::Button::Right, skipChapterMs);
    const bool skipBack = mappedInput.wasLongPressed(MappedInputManager::Button::PageBack, skipChapterMs) ||
                          mappedInput.wasLongPressed(MappedInputManager::Button::Left, skipChapterMs);
    if (skipFwd || skipBack) {
      const auto& chapters = xtc->getChapters();
      // Find the chapter containing the current page.
      int chapterIndex = 0;
      for (int i = 0; i < static_cast<int>(chapters.size()); i++) {
        if (currentPage >= chapters[i].startPage && currentPage <= chapters[i].endPage) {
          chapterIndex = i;
          break;
        }
      }
      if (skipFwd && chapterIndex + 1 < static_cast<int>(chapters.size())) {
        currentPage = chapters[chapterIndex + 1].startPage;
      } else if (skipBack && chapterIndex > 0) {
        currentPage = chapters[chapterIndex - 1].startPage;
      }
      requestUpdate();
      return;
    }
  } else if (SETTINGS.longPressChapterSkip) {
    // Consume the long press even with no chapters so the release does not turn a page.
    mappedInput.wasLongPressed(MappedInputManager::Button::PageForward, skipChapterMs);
    mappedInput.wasLongPressed(MappedInputManager::Button::Right, skipChapterMs);
    mappedInput.wasLongPressed(MappedInputManager::Button::PageBack, skipChapterMs);
    mappedInput.wasLongPressed(MappedInputManager::Button::Left, skipChapterMs);
  }

  // Double press: skip 10 pages forward or backward.
  if (SETTINGS.doublePressPageSkip) {
    const bool skipFwd = mappedInput.wasDoublePressed(MappedInputManager::Button::PageForward, doubleClickWindowMs) ||
                         mappedInput.wasDoublePressed(MappedInputManager::Button::Right, doubleClickWindowMs);
    const bool skipBack = mappedInput.wasDoublePressed(MappedInputManager::Button::PageBack, doubleClickWindowMs) ||
                          mappedInput.wasDoublePressed(MappedInputManager::Button::Left, doubleClickWindowMs);
    if (skipFwd || skipBack) {
      if (skipFwd) {
        currentPage += kTenPageSkip;
        if (currentPage >= xtc->getPageCount()) currentPage = xtc->getPageCount() - 1;
      } else {
        currentPage -= (currentPage >= kTenPageSkip) ? kTenPageSkip : currentPage;
      }
      requestUpdate();
      return;
    }
  }

  auto [prevTriggered, nextTriggered] = ReaderUtils::detectPageTurn(mappedInput);

  // Deferred page turn: fires after the double-press window expires when the button is released.
  if (!prevTriggered && !nextTriggered && SETTINGS.doublePressPageSkip) {
    if (!mappedInput.isPressed(MappedInputManager::Button::PageBack) &&
        !mappedInput.isPressed(MappedInputManager::Button::Left))
      prevTriggered = mappedInput.wasDoublePressExpired(MappedInputManager::Button::PageBack) ||
                      mappedInput.wasDoublePressExpired(MappedInputManager::Button::Left);
    if (!mappedInput.isPressed(MappedInputManager::Button::PageForward) &&
        !mappedInput.isPressed(MappedInputManager::Button::Right))
      nextTriggered = mappedInput.wasDoublePressExpired(MappedInputManager::Button::PageForward) ||
                      mappedInput.wasDoublePressExpired(MappedInputManager::Button::Right);
  }

  if (!prevTriggered && !nextTriggered) {
    return;
  }

  // Handle end of book
  if (currentPage >= xtc->getPageCount()) {
    currentPage = xtc->getPageCount() - 1;
    requestUpdate();
    return;
  }

  if (prevTriggered) {
    if (currentPage >= 1) currentPage--;
    requestUpdate();
  } else {
    currentPage++;
    if (currentPage >= xtc->getPageCount()) currentPage = xtc->getPageCount() - 1;
    requestUpdate();
  }
}

void XtcReaderActivity::render(RenderLock&&) {
  if (!xtc) {
    return;
  }

  // Bounds check
  if (currentPage >= xtc->getPageCount()) {
    // Show end of book screen
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  renderPage();
  saveProgress();
}

void XtcReaderActivity::renderPage() {
  const uint16_t pageWidth = xtc->getPageWidth();
  const uint16_t pageHeight = xtc->getPageHeight();
  const uint8_t bitDepth = xtc->getBitDepth();

  // Calculate buffer size for one page
  // XTG (1-bit): Row-major, ((width+7)/8) * height bytes
  // XTH (2-bit): Two bit planes, column-major, ((width * height + 7) / 8) * 2 bytes
  size_t pageBufferSize;
  if (bitDepth == 2) {
    pageBufferSize = ((static_cast<size_t>(pageWidth) * pageHeight + 7) / 8) * 2;
  } else {
    pageBufferSize = ((pageWidth + 7) / 8) * pageHeight;
  }

  // Allocate page buffer
  uint8_t* pageBuffer = static_cast<uint8_t*>(malloc(pageBufferSize));
  if (!pageBuffer) {
    LOG_ERR("XTR", "Failed to allocate page buffer (%lu bytes)", pageBufferSize);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_MEMORY_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Load page data
  size_t bytesRead = xtc->loadPage(currentPage, pageBuffer, pageBufferSize);
  if (bytesRead == 0) {
    LOG_ERR("XTR", "Failed to load page %lu", currentPage);
    free(pageBuffer);
    renderer.clearScreen();
    renderer.drawCenteredText(UI_12_FONT_ID, 300, tr(STR_PAGE_LOAD_ERROR), true, EpdFontFamily::BOLD);
    renderer.displayBuffer();
    return;
  }

  // Clear screen first
  renderer.clearScreen();

  // Copy page bitmap using GfxRenderer's drawPixel
  // XTC/XTCH pages are pre-rendered with status bar included, so render full page
  const uint16_t maxSrcY = pageHeight;

  if (bitDepth == 2) {
    // XTH 2-bit mode: Two bit planes, column-major order
    // - Columns scanned right to left (x = width-1 down to 0)
    // - 8 vertical pixels per byte (MSB = topmost pixel in group)
    // - First plane: Bit1, Second plane: Bit2
    // - Pixel value = (bit1 << 1) | bit2 (mapped to 0=Black, 3=White)
    // - Grayscale: 0=Black, 1=Dark Grey, 2=Light Grey, 3=White

    const size_t planeSize = (static_cast<size_t>(pageWidth) * pageHeight + 7) / 8;
    const uint8_t* plane1 = pageBuffer;              // Bit1 plane
    const uint8_t* plane2 = pageBuffer + planeSize;  // Bit2 plane
    const size_t colBytes = (pageHeight + 7) / 8;    // Bytes per column (100 for 800 height)

    // Lambda to get pixel value at (x, y) — used for BW render and debug stats
    auto getPixelValue = [&](uint16_t x, uint16_t y) -> uint8_t {
      const size_t colIndex = pageWidth - 1 - x;
      const size_t byteInCol = y / 8;
      const size_t bitInByte = 7 - (y % 8);
      const size_t byteOffset = colIndex * colBytes + byteInCol;
      const uint8_t bit1 = (plane1[byteOffset] >> bitInByte) & 1;
      const uint8_t bit2 = (plane2[byteOffset] >> bitInByte) & 1;
      return 3 - ((bit2 << 1) | bit1); // Total Inversion Fix
    };

    // Context + callback for renderGrayscale. Pixel selection adapts to the render mode set
    // by renderGrayscale before each pass:
    //   GRAY2_LSB  (factory BW RAM):   pv>=2 — LightGrey(2) and Black(3) → bit1=1
    //   GRAY2_MSB  (factory RED RAM):  pv&1  — DarkGrey(1) and Black(3)  → bit0=1
    //   GRAYSCALE_LSB (diff BW RAM):   pv==1 — DarkGrey only
    //   GRAYSCALE_MSB (diff RED RAM):  pv==1||pv==2 — DarkGrey and LightGrey
    struct XtcGrayCtx {
      const uint8_t* plane1;
      const uint8_t* plane2;
      uint16_t pageWidth, pageHeight;
      size_t colBytes;
    };
    XtcGrayCtx xtcCtx{plane1, plane2, pageWidth, pageHeight, colBytes};
    const auto xtcGrayFn = [](const GfxRenderer& r, const void* raw) {
      const auto* c = static_cast<const XtcGrayCtx*>(raw);
      DirectPixelWriter pw;
      pw.init(r);

      const size_t colStride = c->colBytes;
      const size_t initialOffset = (c->pageWidth - 1) * colStride;

      for (uint16_t y = 0; y < c->pageHeight; y++) {
        const size_t byteInCol = y >> 3;
        const uint8_t bitInByte = 7 - (y & 7);
        const uint8_t* p1 = c->plane1 + initialOffset + byteInCol;
        const uint8_t* p2 = c->plane2 + initialOffset + byteInCol;

        pw.beginRow(y);
        for (uint16_t x = 0; x < c->pageWidth; x++) {
          const uint8_t b1 = (*p1 >> bitInByte) & 1;
          const uint8_t b2 = (*p2 >> bitInByte) & 1;
          const uint8_t pv = 3 - ((b2 << 1) | b1);
          pw.writePixel(x, pv);
          
          p1 -= colStride;
          p2 -= colStride;
        }
      }
    };

    const bool useFactory = SETTINGS.factoryLutImages;

    // Fast Inversion Pass Logic:
    // We use the "Seamless" technique: a quick negative-positive flip to clear ghosting,
    // followed by a high-speed grayscale overlay using the FactoryFast LUT (no white flash).
    
    if (useFactory) {
      // 1. Show next page BW base
      // (Already in framebuffer via renderGrayscale calls below or manual draw)
    } else {
      // Manual 2-pass Differential rendering logic...
      renderer.clearScreen();
      DirectPixelWriter pw;
      pw.init(renderer);
      for (uint16_t y = 0; y < pageHeight; y++) {
        pw.beginRow(y);
        for (uint16_t x = 0; x < pageWidth; x++) {
          if (getPixelValue(x, y) < 3) pw.writePixel(x, 0);
        }
      }
    }

    if (useFactory) {
      const bool fullRefresh = (pagesUntilFullRefresh <= 1);
      
      if (fullRefresh) {
        // Pre-flash to white to guarantee clean starting state for quality LUT
        renderer.clearScreen();
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
        pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();
      } else {
        pagesUntilFullRefresh--;
      }

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAY2_LSB);
      xtcGrayFn(renderer, &xtcCtx);
      renderer.copyGrayscaleLsbBuffers();

      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAY2_MSB);
      xtcGrayFn(renderer, &xtcCtx);
      renderer.copyGrayscaleMsbBuffers();

      extern const unsigned char lut_factory_fast[];
      extern const unsigned char lut_factory_quality[];
      renderer.displayGrayBuffer(fullRefresh ? lut_factory_quality : lut_factory_fast, true);
      renderer.setRenderMode(GfxRenderer::BW);
    } else {
      // Apply grayscale overlay Differential
      renderer.renderGrayscale(GfxRenderer::GrayscaleMode::Differential, xtcGrayFn, &xtcCtx);
      
      // Re-render BW to framebuffer for next turn consistency
      renderer.clearScreen();
      DirectPixelWriter pw;
      pw.init(renderer);
      for (uint16_t y = 0; y < pageHeight; y++) {
        pw.beginRow(y);
        for (uint16_t x = 0; x < pageWidth; x++) {
          if (getPixelValue(x, y) < 3) pw.writePixel(x, 0);
        }
      }
      renderer.cleanupGrayscaleWithFrameBuffer();
    }

    free(pageBuffer);
    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit %s)", currentPage + 1, xtc->getPageCount(),
            useFactory ? "factory" : "differential");
    return;
  } else {
    // 1-bit mode: 8 pixels per byte, MSB first
    const size_t srcRowBytes = (pageWidth + 7) / 8;

    DirectPixelWriter pw;
    pw.init(renderer);
    for (uint16_t srcY = 0; srcY < maxSrcY; srcY++) {
      const size_t srcRowStart = srcY * srcRowBytes;
      pw.beginRow(srcY);
      for (uint16_t srcX = 0; srcX < pageWidth; srcX++) {
        const size_t srcByte = srcRowStart + srcX / 8;
        const size_t srcBit = 7 - (srcX % 8);
        const bool isBlack = !((pageBuffer[srcByte] >> srcBit) & 1);

        if (isBlack) {
          pw.writePixel(srcX, 0); // Black
        }
      }
    }
  }
  // White pixels are already cleared by clearScreen()

  free(pageBuffer);

  // Use refresh cycle: periodic HALF_REFRESH to clear ghosting per user setting
  ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

  LOG_DBG("XTR", "Rendered page %lu/%lu (%u-bit)", currentPage + 1, xtc->getPageCount(), bitDepth);
}

void XtcReaderActivity::saveProgress() const {
  FsFile f;
  if (Storage.openFileForWrite("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    data[0] = currentPage & 0xFF;
    data[1] = (currentPage >> 8) & 0xFF;
    data[2] = (currentPage >> 16) & 0xFF;
    data[3] = (currentPage >> 24) & 0xFF;
    f.write(data, 4);
    f.close();
  }
}

void XtcReaderActivity::loadProgress() {
  FsFile f;
  if (Storage.openFileForRead("XTR", xtc->getCachePath() + "/progress.bin", f)) {
    uint8_t data[4];
    if (f.read(data, 4) == 4) {
      currentPage = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
      LOG_DBG("XTR", "Loaded progress: page %lu", currentPage);

      // Validate page number
      if (currentPage >= xtc->getPageCount()) {
        currentPage = 0;
      }
    }
    f.close();
  }
}
