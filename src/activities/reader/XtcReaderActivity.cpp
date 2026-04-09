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
#include "ReaderUtils.h"
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

  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
  pagesUntilFullRefresh = SETTINGS.getRefreshFrequency();

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

  // At end of the book, forward button goes home and back button returns to last page
  if (currentPage >= xtc->getPageCount()) {
    if (nextTriggered) {
      onGoHome();
    } else {
      currentPage = xtc->getPageCount() - 1;
      requestUpdate();
    }
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

  if (bitDepth == 2) {
    const uint8_t* plane1 = pageBuffer;
    const uint8_t* plane2 = pageBuffer + (pageBufferSize / 2);
    renderer.displayXtchPlanes(plane1, plane2, pageWidth, pageHeight);

    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

    free(pageBuffer);
    LOG_DBG("XTR", "Rendered page %lu/%lu (2-bit factory)", currentPage + 1, xtc->getPageCount());
    return;
  } else {
    renderer.displayXtcBwPage(pageBuffer, pageWidth, pageHeight);

    ReaderUtils::displayWithRefreshCycle(renderer, pagesUntilFullRefresh);

    free(pageBuffer);
    LOG_DBG("XTR", "Rendered page %lu/%lu (1-bit)", currentPage + 1, xtc->getPageCount());
    return;
  }

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
