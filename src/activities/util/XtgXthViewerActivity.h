#pragma once

#include <string>

#include "../Activity.h"

class XtgXthViewerActivity final : public Activity {
 private:
  std::string filePath;

 public:
  XtgXthViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path);
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
