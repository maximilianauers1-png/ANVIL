#include "settings.hpp"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include "anvil/tolerance.hpp"
#include "anvil/version.hpp"

namespace anvil {

QString Settings::path() {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(dir);
    return dir + "/settings.json";
}

Settings Settings::load() {
    Settings s;
    s.chord_mm = tol::kDefaultChordMm;
    QFile f(path());
    if (!f.open(QIODevice::ReadOnly)) return s;
    QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    int v = o.value("version").toInt(0);
    // --- migrations: each step upgrades version N -> N+1 (none needed yet) ---
    if (v > kSettingsFormat) return s;  // written by a newer ANVIL: keep defaults, do not overwrite blindly
    s.theme = o.value("theme").toString(s.theme);
    s.unit = o.value("unit").toString(s.unit);
    s.radial_key = o.value("radial_key").toString(s.radial_key);
    s.chord_mm = o.value("chord_mm").toDouble(s.chord_mm);
    if (s.chord_mm < tol::kMinChordMm) s.chord_mm = tol::kDefaultChordMm;
    s.autosave_seconds = o.value("autosave_seconds").toInt(s.autosave_seconds);
    s.undo_limit = o.value("undo_limit").toInt(s.undo_limit);
    s.radial_layout = o.value("radial_layout").toObject().toVariantMap();
    for (const auto& r : o.value("recent").toArray()) s.recent << r.toString();
    return s;
}

void Settings::save() const {
    QJsonObject o;
    o["version"] = kSettingsFormat;
    o["theme"] = theme;
    o["unit"] = unit;
    o["radial_key"] = radial_key;
    o["chord_mm"] = chord_mm;
    o["autosave_seconds"] = autosave_seconds;
    o["undo_limit"] = undo_limit;
    o["radial_layout"] = QJsonObject::fromVariantMap(radial_layout);
    o["recent"] = QJsonArray::fromStringList(recent);
    QSaveFile f(path());  // atomic replace
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson());
        f.commit();
    }
}

}  // namespace anvil
