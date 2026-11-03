// wwe2k15 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <stdexcept>

REXCVAR_DECLARE(std::string, wwe2k15_graphics_backend);

class Wwe2k15App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Wwe2k15App>(new Wwe2k15App(ctx, "wwe2k15",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    const auto& backend = REXCVAR_GET(wwe2k15_graphics_backend);
    if (backend == "any") return;
    if (backend != "d3d12" && backend != "vulkan") {
      throw std::runtime_error("wwe2k15_graphics_backend must be any, d3d12 or vulkan");
    }
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, backend);
    if (!config.graphics) {
      throw std::runtime_error("Requested graphics backend is unavailable in the GPU plugin");
    }
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
