#include "simulator/sim.h"
#include "simulator/sim_gui.hpp"
#include "simulator/sim_files.h"
#include "simulator/sim_carts.h"

#include "simulator/imgui/imgui.hpp"

#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"
#include "gui_internal.hpp"

#include "ImGuiFileDialog.h"

namespace SIM::GUI {
static SDL_Window * sWindow;
static SDL_GLContext sContext;
static ImGuiContext * sImguiContext;
static bool sEnabled = false;
static bool sPauseGameLogic = false;
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

    //ImFontAtlas::ImFontAtlas_AddFontDefault
    //ioptr->Fonts.AddFontDefault(NULL);
}

void Main() {
    SIM_Files_Update();
    SIM_Carts_DrawUI();
    if (SIM_Carts_PickerActive()) {
        return;
    }
    PrjOverlay();

    if(sEnabled) {
        ImGui::Begin("libntr", &sEnabled);

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
    ImGui::NewFrame();
}

void ProcessEvent(SDL_Event * event) {
    if(sEnabled || SIM_Carts_UIActive()) {
        ImGui_ImplSDL2_ProcessEvent(event);
    }
}

void Render() {
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void Toggle() {
    if(sEnabled) {
        sEnabled = false;
    } else {
        sEnabled = true;
    }
}

void Enable() {
    sEnabled = true;
}

bool IsGameLogicPaused() {
    // The game waits while a game is being added from inside it
    return sPauseGameLogic || (SIM_Carts_UIActive() && !SIM_Carts_PickerActive());
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