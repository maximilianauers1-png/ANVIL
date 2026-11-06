// Bridge between QML and the session (command bus). Every UI action ends up
// as Python source submitted here (requirement 2.5).
#pragma once
#include <QDir>
#include <QHash>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector3D>

#include <memory>

#include "anvil/fuzzy.hpp"
#include "anvil/units.hpp"
#include "scene_builder.hpp"
#include "session.hpp"
#include "settings.hpp"

namespace anvil {

class ViewportItem;

class AppController : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList objects READ objects NOTIFY documentChanged)
    Q_PROPERTY(QVariantList history READ history NOTIFY documentChanged)
    Q_PROPERTY(int historyPosition READ historyPosition NOTIFY documentChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY documentChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY documentChanged)
    Q_PROPERTY(QVariantList variables READ variables NOTIFY documentChanged)
    Q_PROPERTY(QStringList selection READ selection WRITE setSelection NOTIFY selectionChanged)
    Q_PROPERTY(QString unit READ unit NOTIFY unitChanged)
    Q_PROPERTY(QString theme READ theme NOTIFY themeChanged)
    Q_PROPERTY(QString mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString display READ display NOTIFY displayChanged)
    Q_PROPERTY(QString scenery READ scenery NOTIFY sceneryChanged)
    Q_PROPERTY(QString consoleText READ consoleText NOTIFY consoleChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QVariantList commands READ commands NOTIFY commandsChanged)
    Q_PROPERTY(QString gpuInfo READ gpuInfo NOTIFY gpuInfoChanged)
    Q_PROPERTY(QString hardwareWarning READ hardwareWarning NOTIFY gpuInfoChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool recoveryAvailable READ recoveryAvailable NOTIFY recoveryChanged)
    Q_PROPERTY(QString recoveryInfo READ recoveryInfo NOTIFY recoveryChanged)
    Q_PROPERTY(QString autosaveInfo READ autosaveInfo NOTIFY autosaveChanged)
    Q_PROPERTY(double startupMs READ startupMs NOTIFY startupMeasured)
    Q_PROPERTY(double chordMm READ chordMm CONSTANT)
    Q_PROPERTY(QString radialKey READ radialKey CONSTANT)
    Q_PROPERTY(QString documentPath READ documentPath NOTIFY documentPathChanged)
    Q_PROPERTY(qulonglong sceneTriangles READ sceneTriangles NOTIFY documentChanged)

  public:
    AppController(SessionOptions opt, std::filesystem::path session_dir, QObject* parent = nullptr);
    ~AppController() override;

    void attachViewport(ViewportItem* v);
    Session& session() { return *session_; }

    QVariantList objects() const { return objects_; }
    QVariantList history() const { return history_; }
    int historyPosition() const { return history_pos_; }
    bool canUndo() const { return can_undo_; }
    bool canRedo() const { return can_redo_; }
    QVariantList variables() const { return variables_; }
    QStringList selection() const { return selection_; }
    void setSelection(const QStringList& s);
    QString unit() const { return QString(unit_info(unit_).symbol); }
    QString theme() const { return theme_; }
    QString mode() const { return mode_; }
    QString display() const { return display_; }
    QString scenery() const { return scenery_; }
    void setMode(const QString& m);
    QString consoleText() const { return console_; }
    QString lastError() const { return last_error_; }
    bool busy() const { return pending_ > 0; }
    QVariantList commands() const { return commands_; }
    QString gpuInfo() const { return gpu_info_; }
    QString hardwareWarning() const { return hw_warning_; }
    QString version() const;
    bool recoveryAvailable() const { return !recovery_journal_.isEmpty(); }
    QString recoveryInfo() const { return recovery_info_; }
    QString autosaveInfo() const { return autosave_info_; }
    double startupMs() const { return startup_ms_; }
    double chordMm() const { return settings_.chord_mm; }
    QString radialKey() const { return settings_.radial_key; }
    QString documentPath() const { return doc_path_; }
    qulonglong sceneTriangles() const { return scene_tris_; }

    void setStartupMs(double ms);

    // --- commands --------------------------------------------------------------
    Q_INVOKABLE void execute(const QString& source, const QString& origin = "console");
    Q_INVOKABLE void runCommand(const QString& id, const QVector3D& point = {}, bool havePoint = false);
    Q_INVOKABLE QVariantList radialItems(const QVariantList& path, const QString& context) const;
    Q_INVOKABLE void radialPick(const QVariantList& path, const QString& context, const QVector3D& point);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void jumpToHistory(int position);

    // --- properties panel ----------------------------------------------------------
    Q_INVOKABLE QVariantList nodeTree(const QString& name) const;
    Q_INVOKABLE void setParam(const QString& name, const QString& path, const QString& key, const QString& text);
    Q_INVOKABLE void setVariable(const QString& name, const QString& text);
    Q_INVOKABLE QString formatLength(double mm) const;
    Q_INVOKABLE QString formatPoint(const QVector3D& p) const;
    Q_INVOKABLE QString validateField(const QString& text, const QString& dim = "Length") const;  // "" or an error
    /// Live ghost of an object with one parameter changed (no document change).
    Q_INVOKABLE void previewParam(const QString& name, const QString& path, const QString& key, const QString& text);
    Q_INVOKABLE void toggleVisible(const QString& name);
    Q_INVOKABLE void renameObject(const QString& from, const QString& to);
    Q_INVOKABLE void pick(const QString& name, bool additive);

    // --- command bar ---------------------------------------------------------------
    Q_INVOKABLE QVariantList fuzzy(const QString& query) const;
    Q_INVOKABLE QVariantMap checkSyntax(const QString& source);
    Q_INVOKABLE QVariantList complete(const QString& source);
    Q_INVOKABLE void preview(const QString& source);
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE QString signature(const QString& source);

    // --- UI state as Python commands ------------------------------------------------
    Q_INVOKABLE void requestUnit(const QString& symbol);
    Q_INVOKABLE void requestTheme(const QString& name);
    Q_INVOKABLE void requestDisplay(const QString& mode);
    Q_INVOKABLE void requestScenery(const QString& name);
    Q_INVOKABLE void requestView(const QString& name);
    Q_INVOKABLE void requestViewDirection(const QVector3D& d);

    // --- files ---------------------------------------------------------------------
    Q_INVOKABLE QVariantMap checkForExport(const QStringList& names, double chordMm);
    Q_INVOKABLE void exportStl(const QStringList& names, const QString& path, const QString& unit, double chordMm,
                               bool ascii);
    Q_INVOKABLE QString dropFiles(const QList<QUrl>& urls);
    Q_INVOKABLE void saveDocument(const QString& path);
    Q_INVOKABLE void openDocument(const QString& path);
    Q_INVOKABLE QString urlToPath(const QUrl& u) const { return u.toLocalFile(); }
    Q_INVOKABLE QString defaultExportPath() const;

    // --- recovery ------------------------------------------------------------------
    Q_INVOKABLE void recover();
    Q_INVOKABLE void discardRecovery();
    Q_INVOKABLE void saveRadialLayout(const QVariantMap& layout);
    Q_INVOKABLE QVariantMap radialLayout() const { return radial_layout_; }
    Q_INVOKABLE void resetRadialLayout();
    Q_INVOKABLE QString commandLabel(const QString& id) const;
    Q_INVOKABLE void appendConsole(const QString& text);
    // sketches
    /// "12.5" (display unit) or "3 cm" -> mm; NaN when the text is not a plain length.
    Q_INVOKABLE double parseLength(const QString& text) const;
    Q_INVOKABLE QString objectKind(const QString& name) const;
    Q_INVOKABLE QVariantMap sketchInfo(const QString& name) const;  // points/curves/dims in world space + solver state
    Q_INVOKABLE void sketchDrag(const QString& sketch, const QString& point, double x, double y);
    Q_INVOKABLE void sketchSetDim(const QString& sketch, const QString& constraint, const QString& text);
    Q_INVOKABLE void sketchConstrain(const QString& sketch, const QString& kind, const QStringList& refs);
    Q_INVOKABLE void sketchDimension(const QString& sketch, const QString& kind, const QStringList& refs, const QString& text);
    Q_INVOKABLE void sketchDelete(const QString& sketch, const QStringList& ids);
    Q_INVOKABLE QString sketchTarget(const QString& sketch) const;
    /// Voxel size (mm) of a sculpt object, 0 if it is not one.
    Q_INVOKABLE double sculptVoxel(const QString& name) const;  // part the sketch was drawn on ("" = unknown)

  signals:
    void documentChanged();
    void selectionChanged();
    void unitChanged();
    void themeChanged();
    void modeChanged();
    void displayChanged();
    void sceneryChanged();
    void consoleChanged();
    void lastErrorChanged();
    void busyChanged();
    void commandsChanged();
    void gpuInfoChanged();
    void recoveryChanged();
    void autosaveChanged();
    void startupMeasured();
    void documentPathChanged();
    void commandFinished(bool ok, const QString& error, const QString& origin);
    void uiToolRequested(const QString& tool);  // "export_dialog", "move_tool", ...
    void viewRequested(const QString& name, const QVector3D& dir);
    void previewError(const QString& error);

  private:
    QString uniqueName(const QString& base) const;
    Dim paramDim(const QString& name, const QString& path, const QString& key) const;
    void onSketchDrawn(const QString& kind, const QString& plane, double offset, const QVariantList& pts, bool closed,
                       const QString& on);
    void onToolsPicked(const QString& target, const QStringList& tools);
    QHash<QString, QString> sketch_on_;  // sketch -> part it was drawn on (cut target)
    QString select_after_;               // select this object once it exists
    void onDocumentChanged();
    void refreshModels();
    void handleUiRequest(const QString& json);
    void loadCommands();
    void detectHardware();
    void autosave();
    void findRecovery();
    QString paramLiteral(const QString& text, Dim dim, QString* error) const;
    QString selectionExpr(const QStringList& names) const;

    SessionOptions opt_;
    std::filesystem::path session_dir_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<SceneBuilder> builder_;
    ViewportItem* viewport_ = nullptr;
    Settings settings_;

    QVariantList objects_, history_, variables_, commands_;
    QVariantMap radial_layout_, default_layout_;
    std::vector<FuzzyEntry> fuzzy_;
    QMap<QString, QVariantMap> command_meta_;
    int history_pos_ = 0;
    bool can_undo_ = false, can_redo_ = false;
    QStringList selection_;
    LengthUnit unit_ = LengthUnit::Millimeter;
    QString theme_ = "dark";
    QString mode_ = "Design";
    QString display_ = "shaded";
    QString scenery_ = "sunset";
    QString console_, last_error_, gpu_info_, hw_warning_;
    int pending_ = 0;
    QString recovery_journal_, recovery_info_, autosave_info_, doc_path_;
    double startup_ms_ = 0;
    qulonglong scene_tris_ = 0;
    std::uint64_t saved_revision_ = 0;
    QTimer autosave_timer_;
    std::uint64_t preview_seq_ = 0;
};

}  // namespace anvil
