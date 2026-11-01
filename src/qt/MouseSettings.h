#pragma once
//
// MouseSettings.h - the user-configurable mouse behaviour (Edit > Preferences
// > Mouse) and the pan/zoom policy applied when navigating between images.
// Every default equals wiv's behaviour before these were configurable.
//
namespace givqt
{

struct MouseSettings
{
    // Plain wheel (Ctrl+wheel always zooms; Alt+wheel scales quivers).
    enum class Wheel { Zoom, Navigate };
    // Back / forward mouse buttons.
    enum class SideButtons { Navigate, StepZoom, Nothing };

    Wheel wheel = Wheel::Zoom;
    SideButtons sideButtons = SideButtons::Navigate;
    bool swapSideButtons = false;    // back = next image / step zoom in
    bool tiltNavigates = true;       // horizontal wheel tilt pages through images
    bool doubleClickToggles = true;  // left/middle double-click: 1:1 <-> fit
};

// What happens to pan/zoom when another image is shown.
enum class NavigationView
{
    Fit,     // re-fit every image (Auto Fit on)
    PerFile, // restore the view each file was left at, else fit
    Keep     // keep the current zoom and pan (Auto Fit off)
};

} // namespace givqt
