#include "SystemInformationActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "SystemStatus.h"
#include "components/UITheme.h"
#include "fontIds.h"

void SystemInformationActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

void SystemInformationActivity::onExit() { Activity::onExit(); }

void SystemInformationActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void SystemInformationActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 tr(STR_SYSTEM_INFO), CROSSPOINT_VERSION);

  const auto status = SystemStatus::collect();

  // Layout: label on the left, value on the right of the midpoint
  const int leftX = metrics.verticalSpacing * 3;
  const int valueX = pageWidth / 2;
  const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int startY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing * 3;

  auto drawRow = [&](int row, const char* label, const std::string& value) {
    const int y = startY + row * (lineH + metrics.verticalSpacing);
    renderer.drawText(UI_10_FONT_ID, leftX, y, label, true, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, valueX, y, value.c_str());
  };

  drawRow(0, "Version", status.version);
  drawRow(1, "Free heap", std::to_string(status.freeHeapBytes / 1024) + " KB");

  const uint32_t h = status.uptimeSeconds / 3600;
  const uint32_t m = (status.uptimeSeconds % 3600) / 60;
  const uint32_t s = status.uptimeSeconds % 60;
  char uptimeBuf[16];
  snprintf(uptimeBuf, sizeof(uptimeBuf), "%uh %02um %02us", h, m, s);
  drawRow(2, "Uptime", uptimeBuf);

  std::string wifiLabel = status.wifiMode;
  if (status.rssi != 0) {
    wifiLabel += " (" + std::to_string(status.rssi) + " dBm)";
  }
  drawRow(3, "WiFi", wifiLabel);
  drawRow(4, "IP address", status.ip);
  drawRow(5, "MAC address", status.macAddress);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
