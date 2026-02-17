#pragma once

struct GLFWwindow;
struct ImFont;

namespace SR {

// Font pointers (set during LoadFonts)
struct Fonts
{
    static inline ImFont* regular = nullptr;
    static inline ImFont* bold    = nullptr;
    static inline ImFont* italic  = nullptr;
    static inline ImFont* mono    = nullptr;
    static inline ImFont* icons   = nullptr;
};

// Load fonts from resources/fonts/ relative to executable
void loadFonts();

// Apply dark theme with accent color
void setupStyle();

// Enable Windows dark mode title bar
void enableDarkTitleBar(GLFWwindow* window);

// Draw a custom panel header: bold title + close (X) button.
// Returns true if close was clicked. Call after ImGui::Begin().
bool panelHeader(const char* title, bool& visible);

} // namespace SR
