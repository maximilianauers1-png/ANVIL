// User settings (JSON with a format version and forward migrations, ADR 0014).
#pragma once
#include <QString>
#include <QVariantMap>

namespace anvil {

struct Settings {
    int version = 1;
    QString theme = "studio";     // dark controls, light scene (user preference, M1 feedback)
    QString scenery = "sunset";   // Studio background: none | sunset | fjord | night
    QString display = "shaded";   // or "mesh" (triangle edges overlaid)
    QString unit = "mm";
    QString radial_key = "Space";
    bool snap = true;                 // snapping in move / draw tools
    double chord_mm = 0.01;           // viewport + export default (tol::kDefaultChordMm)
    int autosave_seconds = 60;
    int undo_limit = 1000;
    QVariantMap radial_layout;        // overrides of the default layout (empty = default)
    QStringList recent;

    static QString path();            // <AppConfigLocation>/settings.json
    static Settings load();           // migrates older versions, falls back to defaults on errors
    void save() const;
};

}  // namespace anvil
