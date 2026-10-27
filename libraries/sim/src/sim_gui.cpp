#include "simulator/sim.h"
#include <cfloat>
#include <cstdio>
#include "simulator/sim_gui.hpp"
#include "simulator/sim_files.h"
#include "simulator/sim_carts.h"
#include "simulator/sim_settings.hpp"
#include "simulator/sim_ui.hpp"
#include "simulator/simvariables.h"

#include "simulator/imgui/imgui.hpp"

#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "gui_internal.hpp"

#include "ImGuiFileDialog.h"

namespace SIM::GUI {
static SDL_Window * sWindow;
static SDL_GLContext sContext;
static ImGuiContext * sImguiContext;
static bool sEnabled = false; // the developer windows
static bool sPauseGameLogic = false;
static bool sWaitForRelease = false; // a menu closed: the game waits until A/B/Start are let go
static constexpr ImVec2 sButtonSize = {100, 20};
static float sUiScale = 1.0f;

static bool sShowAppConfig = false;
static bool sShowAppPad = false;
static bool sShowAppNet = false;
static bool sShowImGuiDemo = false;
static bool sShowWindowPrjSpecific = false;

static std::function<void(std::string)> sFileDialogCallback = nullptr;

void PrjMain(bool *) __attribute__((weak));

// Drawn every frame, whether or not the settings GUI is open (notifications, indicators)
void PrjOverlay() __attribute__((weak));
void __attribute__((weak)) PrjOverlay() {
}

void __attribute__((weak)) PrjMain(bool * openState) {
    ImGui::OpenPopup("Application-Specific GUI");
    if(ImGui::BeginPopupModal("Application-Specific GUI", openState, ImGuiWindowFlags_Modal | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::Text("No application-specific GUI has been created for this application.");
        ImGui::EndPopup();
    }
}

void Init(SDL_Window * window, SDL_GLContext context) {
    sWindow = window;
    sContext = context;

    sImguiContext = ImGui::CreateContext(NULL);
    static std::string sIniPath = SIM_SettingsPath("imgui.ini");
    ImGui::GetIO().IniFilename = sIniPath.c_str();

    ImGuiIO igIO = ImGui::GetIO();
	igIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;       // Enable Keyboard Controls
	igIO.DisplaySize.x = (float)(SIM_NDS_SCREEN_WIDTH*SIM_NDS_WINDOW_SCALE_FACTOR);
	igIO.DisplaySize.y = (float)(SIM_NDS_SCREEN_HEIGHT*2*SIM_NDS_WINDOW_SCALE_FACTOR);



	ImGui_ImplSDL2_InitForOpenGL(sWindow, sContext);
#ifdef SIM_GLES
	const char * glsl_version = "#version 300 es";
#else
	const char * glsl_version = "#version 420";
#endif
  	ImGui_ImplOpenGL3_Init(glsl_version);

    ImGui::StyleColorsDark(NULL);

#ifdef SDK_BUILD_ANDROID
    // Phone/handheld panels are dense: scale the settings GUI so it can be used by touch
    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    float uiScale = (float)windowHeight / 540.0f;
    if (uiScale < 1.0f) {
        uiScale = 1.0f;
    }
    ImGui::GetStyle().ScaleAllSizes(uiScale);
    ImGui::GetIO().FontGlobalScale = uiScale;
    sUiScale = uiScale;
#endif

    SIM::UI::Init();
}

static bool MenuActive() {
    return SIM::Settings::IsOpen() || SIM_Carts_UIActive();
}

// Settings > Advanced > Frame timings on screen: where each frame's time goes, over the
// game (top-left corner of the main window)
static void PerfOverlay() {
    if (!SIM_GetConfigPtr()->perfOverlay) {
        return;
    }
    SIM_FrameTimings t;
    SIM_GetFrameTimings(&t);
    float other = t.drawMs - (t.g2Ms + t.uploadMs + t.secondaryMs + t.guiMs);
    if (other < 0.0f) {
        other = 0.0f;
    }
    char lines[3][200];
    snprintf(lines[0], sizeof(lines[0]), "%.0f fps shown, game at %.0f/60  (3D draws %.0f, layer uploads %.1f)",
             t.shownPerSecond, t.vblanksPerSecond, t.draws3d, t.uploads2d);
    snprintf(lines[1], sizeof(lines[1]),
             "CPU ms  game %.1f  3D %.1f  2D %.1f  upload %.1f  2nd screen %.1f (gear %.1f, show %.1f)  menus %.1f  "
             "other %.1f  wait %.1f  swap %.1f",
             t.gameMs, t.g3SubmitMs, t.g2Ms, t.uploadMs, t.secondaryMs, t.companionMs, t.secondarySwapMs, t.guiMs,
             other, t.capWaitMs, t.swapMs);
    if (t.gpu3dMs >= 0.0f) {
        snprintf(lines[2], sizeof(lines[2]), "GPU ms  3D %.1f  top %.1f  bottom %.1f", t.gpu3dMs, t.gpuMainMs,
                 t.gpuSecondaryMs);
    } else {
        snprintf(lines[2], sizeof(lines[2]), "GPU ms  (no timer queries on this GPU)");
    }

    ImFont *font = SIM::UI::RegularFont();
    const float px = SIM::UI::S(12.0f);
    const float pad = SIM::UI::S(5.0f);
    float width = 0.0f, height = pad;
    for (const char *line : lines) {
        ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0.0f, line);
        width = size.x > width ? size.x : width;
        height += size.y;
    }
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), ImVec2(width + pad * 2, height + pad), IM_COL32(0, 0, 0, 170));
    float y = pad;
    for (const char *line : lines) {
        draw->AddText(font, px, ImVec2(pad, y), IM_COL32(235, 240, 245, 255), line);
        y += font->CalcTextSizeA(px, FLT_MAX, 0.0f, line).y;
    }
}

void Main() {
    static bool sMenuWasActive = false;
    const bool menuActive = MenuActive();
    if (sMenuWasActive && !menuActive) {
        sWaitForRelease = true;
    }
    sMenuWasActive = menuActive;

    SIM_Files_Update();
    if (SIM_Carts_UIActive()) {
        // The game picker, or adding a game
        SIM_Carts_DrawUI();
        return;
    }
    PrjOverlay();
    if (!MenuActive()) {
        PerfOverlay();
    }

    if (SIM::Settings::IsOpen()) {
        SIM::Settings::Draw();
        return;
    }

    if(sEnabled) {
        ImGui::Begin("libntr", &sEnabled);

        if (ImGui::Button("Back to the settings")) {
            sEnabled = false;
            SIM::Settings::Open();
        }

        ImGui::Text("Git rev: %s", SIM_GetLibntrGitHash());

        float renderTimeMs = static_cast<float>(SIM_GetRenderFrameTime()) / 1000000.0f;
        ImGui::Text("Render time: %.3fms", renderTimeMs);

        float frameTimeMs = static_cast<float>(SIM_GetFullFrameTime()) / 1000000.0f;
        ImGui::Text("%.0ffps", 1000.0f / frameTimeMs);

        SIM_FrameTimings timings;
        SIM_GetFrameTimings(&timings);
        ImGui::Text("Game %.1f  3D %.1f  Draw %.1f", timings.gameMs, timings.g3SubmitMs, timings.drawMs);
        ImGui::Text("Wait %.1f  Swap %.1f (ms)", timings.capWaitMs, timings.swapMs);


        AppButton("Config", &sShowAppConfig, AppConfigInit, AppConfigMain);
        AppButton("Input", &sShowAppPad, AppPadInit, AppPadMain);
        AppButton("Multiplayer", &sShowAppNet, AppNetInit, AppNetMain);
        AppButton("ImGui Demo", &sShowImGuiDemo, nullptr, ImGui::ShowDemoWindow);
        AppButton("Application", &sShowWindowPrjSpecific, nullptr, PrjMain);

        if (ImGuiFileDialog::Instance()->Display("ChooseFileDlgKey", ImGuiWindowFlags_NoCollapse, ImVec2(200, 200))) {
          if (ImGuiFileDialog::Instance()->IsOk()) { // action if OK
            std::string filePathName = ImGuiFileDialog::Instance()->GetFilePathName();
            std::string filePath = ImGuiFileDialog::Instance()->GetCurrentPath();

            // FilePathName will be the full path to the file selected
            if(sFileDialogCallback) {
                sFileDialogCallback(filePathName);
            }
          }

          // close
          ImGuiFileDialog::Instance()->Close();
        }

        ImGui::Checkbox("Pause Game Logic", &sPauseGameLogic);

        ImGui::End();
    }
}

void OpenFileDialog(std::function<void(std::string)> callback) {
    sFileDialogCallback = callback;

    IGFD::FileDialogConfig config;
    	config.path = ".";
    ImGuiFileDialog::Instance()->OpenDialog("ChooseFileDlgKey", "Select NDS ROM", ".nds,.srl", config);
}

void NewFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    // The menus take the DS buttons (after the backend, which clears the gamepad flag)
    SIM::UI::FeedPad(MenuActive());
    ImGui::NewFrame();
}

void ProcessEvent(SDL_Event * event) {
    if(sEnabled || MenuActive()) {
        ImGui_ImplSDL2_ProcessEvent(event);
    }
}

void Render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// The settings key (Android: back button, R3)
void Toggle() {
    if (SIM_Carts_UIActive()) {
        SIM_Carts_Back();
    } else if (sEnabled) {
        sEnabled = false;
    } else if (SIM::Settings::IsOpen()) {
        SIM::Settings::Close();
    } else {
        SIM::Settings::Open();
    }
}

void Enable() {
    sEnabled = true;
}

void ShowDeveloperWindows() {
    sEnabled = true;
}

bool IsGameLogicPaused() {
    if (sWaitForRelease) {
        // Don't let the button that closed a menu reach the game
        if ((s_reg_PAD_KEYINPUT & 0x00B) == 0x00B) {
            sWaitForRelease = false;
        } else {
            return true;
        }
    }
    // The game waits while a menu is open
    return sPauseGameLogic || SIM::Settings::IsOpen() || (SIM_Carts_UIActive() && !SIM_Carts_PickerActive());
}

// Generic button to open another GUI applet
void AppButton(const char * label, bool * state, void (*initFunc)(void), void (*appFunc)(bool *)) {
    if(ImGui::Button(label, ImVec2(sButtonSize.x * sUiScale, sButtonSize.y * sUiScale))) {
        if(*state) {
            *state = false;
        } else {
            if(initFunc) {
                initFunc();
            }
            *state = true;
        }
    }

    if(*state) {
        appFunc(state);
    }
}

}