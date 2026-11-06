// QML item hosting the Vulkan viewport: camera navigation, picking, snapping,
// radial-menu gestures, move tool, frame-time statistics and benchmarks.
#pragma once
#include <QColor>
#include <QElapsedTimer>
#include <QPointF>
#include <QQuickItem>
#include <QVariantList>
#include <QVector3D>

#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "anvil/radial.hpp"
#include "anvil/sketch.hpp"
#include "anvil/snap.hpp"
#include "scene_builder.hpp"

namespace anvil {

class ViewportItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QColor backgroundColor MEMBER bg_ NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER grid_ NOTIFY styleChanged)
    Q_PROPERTY(QColor edgeColor MEMBER edge_ NOTIFY styleChanged)
    Q_PROPERTY(QVariantList objectColors MEMBER object_colors_ NOTIFY styleChanged)
    Q_PROPERTY(bool showGrid MEMBER show_grid_ NOTIFY styleChanged)
    Q_PROPERTY(QColor wireColor MEMBER wire_ NOTIFY styleChanged)
    Q_PROPERTY(bool meshView MEMBER mesh_view_ NOTIFY styleChanged)
    // sculpting (M3)
    Q_PROPERTY(bool sculptMode READ sculptMode WRITE setSculptMode NOTIFY toolChanged)
    Q_PROPERTY(QString sculptBrush MEMBER sculpt_brush_ NOTIFY sculptChanged)
    Q_PROPERTY(double sculptRadius MEMBER sculpt_radius_ NOTIFY sculptChanged)
    Q_PROPERTY(double sculptStrength MEMBER sculpt_strength_ NOTIFY sculptChanged)
    Q_PROPERTY(bool sculptInvert MEMBER sculpt_invert_ NOTIFY sculptChanged)
    Q_PROPERTY(QString sculptSymmetry MEMBER sculpt_symmetry_ NOTIFY sculptChanged)
    Q_PROPERTY(double sculptFalloff MEMBER sculpt_falloff_ NOTIFY sculptChanged)
    Q_PROPERTY(QString sculptPattern MEMBER sculpt_pattern_ NOTIFY sculptChanged)
    Q_PROPERTY(double sculptPatternSize MEMBER sculpt_pattern_size_ NOTIFY sculptChanged)
    Q_PROPERTY(QStringList sculptObjects READ sculptObjects NOTIFY sculptObjectsChanged)
    Q_PROPERTY(QPointF brushPos READ brushPos NOTIFY brushChanged)
    Q_PROPERTY(double brushScreenRadius READ brushScreenRadius NOTIFY brushChanged)
    Q_PROPERTY(QString brushTarget READ brushTarget NOTIFY brushChanged)
    Q_PROPERTY(double sculptUpdateMs READ sculptUpdateMs NOTIFY brushChanged)
    Q_PROPERTY(QString scenery MEMBER scenery_ NOTIFY styleChanged)
    Q_PROPERTY(QStringList selection READ selection WRITE setSelection NOTIFY selectionChanged)
    // snapping (read by the QML overlay)
    Q_PROPERTY(bool snapVisible READ snapVisible NOTIFY snapChanged)
    Q_PROPERTY(QPointF snapPos READ snapPos NOTIFY snapChanged)
    Q_PROPERTY(QString snapLabel READ snapLabel NOTIFY snapChanged)
    Q_PROPERTY(QString snapKind READ snapKind NOTIFY snapChanged)
    Q_PROPERTY(QVector3D snapPoint READ snapPoint NOTIFY snapChanged)
    Q_PROPERTY(QString hoverObject READ hoverObject NOTIFY snapChanged)
    // tools
    Q_PROPERTY(QString tool READ tool NOTIFY toolChanged)
    Q_PROPERTY(QString toolHint READ toolHint NOTIFY toolChanged)
    Q_PROPERTY(bool hasBasePoint READ hasBasePoint NOTIFY toolChanged)
    Q_PROPERTY(QPointF basePos READ basePos NOTIFY snapChanged)
    // drawing tool (polyline / rectangle / circle on a plane)
    Q_PROPERTY(QVariantList drawPoints READ drawPoints NOTIFY drawChanged)
    Q_PROPERTY(QPointF drawCursor READ drawCursor NOTIFY drawChanged)
    Q_PROPERTY(QString drawKind READ drawKind NOTIFY drawChanged)
    Q_PROPERTY(QString drawGuide READ drawGuide NOTIFY drawChanged)
    Q_PROPERTY(double drawSegment READ drawSegment NOTIFY drawChanged)
    Q_PROPERTY(QString drawPlane READ drawPlane NOTIFY drawChanged)
    Q_PROPERTY(QPointF drawCursorSketch READ drawCursorSketch NOTIFY drawChanged)
    Q_PROPERTY(QVariantList drawPointsSketch READ drawPointsSketch NOTIFY drawChanged)
    // 'Cut with...' picking
    Q_PROPERTY(QStringList pickedTools READ pickedTools NOTIFY toolChanged)
    Q_PROPERTY(QString pickTarget READ pickTarget NOTIFY toolChanged)
    // camera basis for the ViewCube
    Q_PROPERTY(QVector3D camRight READ camRight NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D camUp READ camUp NOTIFY cameraChanged)
    Q_PROPERTY(QVector3D camForward READ camForward NOTIFY cameraChanged)
    Q_PROPERTY(bool ortho READ ortho NOTIFY cameraChanged)
    Q_PROPERTY(double pixelSizeMm READ pixelSizeMm NOTIFY cameraChanged)
    // radial menu state
    Q_PROPERTY(bool radialActive READ radialActive NOTIFY radialChanged)
    Q_PROPERTY(QPointF radialCenter READ radialCenter NOTIFY radialChanged)
    Q_PROPERTY(int radialHighlight READ radialHighlight NOTIFY radialChanged)
    Q_PROPERTY(QVariantList radialPath READ radialPath NOTIFY radialChanged)
    Q_PROPERTY(QString radialContext READ radialContext NOTIFY radialChanged)
    // statistics
    Q_PROPERTY(double frameP50 READ frameP50 NOTIFY statsChanged)
    Q_PROPERTY(double frameP99 READ frameP99 NOTIFY statsChanged)
    Q_PROPERTY(double gpuMs READ gpuMs NOTIFY statsChanged)
    Q_PROPERTY(qulonglong triangles READ triangles NOTIFY statsChanged)
    Q_PROPERTY(qulonglong gpuBytes READ gpuBytes NOTIFY statsChanged)
    Q_PROPERTY(qulonglong vramTotal READ vramTotal NOTIFY statsChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY statsChanged)
    Q_PROPERTY(int msaa READ msaa NOTIFY statsChanged)

  public:
    explicit ViewportItem(QQuickItem* parent = nullptr);
    ~ViewportItem() override;

    void setScene(std::shared_ptr<const Scene> s);
    void setGhost(std::shared_ptr<const MeshCpu> g);
    void setRadialProvider(RadialRingProvider p) { radial_rings_ = std::move(p); }
    /// Refines the radial context from the selection ("selection" -> "sketch" for a selected sketch).
    void setContextResolver(std::function<QString(const QString&)> f) { context_resolver_ = std::move(f); }
    Vec3 radialPointExact() const { return radial_point_exact_; }
    /// World point -> item coordinates (logical pixels).
    QPointF projectPoint(const Vec3& p) const;

    QStringList selection() const { return selection_; }
    QVariantList drawPoints() const;
    QPointF drawCursorSketch() const { return draw_.cursor_valid ? QPointF(draw_.cursor[0], draw_.cursor[1]) : QPointF(); }
    QVariantList drawPointsSketch() const {
        QVariantList l;
        for (const auto& p : draw_.pts) l << QPointF(p[0], p[1]);
        return l;
    }
    QPointF drawCursor() const { return draw_.cursor_valid ? sketchToItem(draw_.cursor[0], draw_.cursor[1]) : QPointF(-1e6, -1e6); }
    QString drawKind() const { return tool_ == "draw" ? draw_.kind : QString(); }
    QString drawGuide() const { return draw_.guide; }
    double drawSegment() const;
    QString drawPlane() const { return draw_.have_plane ? QString(plane_name(draw_.plane)) : QString(); }
    QStringList pickedTools() const { return picked_tools_; }
    QString pickTarget() const { return pick_target_; }
    void setSelection(const QStringList& s);
    bool snapVisible() const { return snap_visible_; }
    QPointF snapPos() const { return {snap_.px / dpr(), snap_.py / dpr()}; }
    QString snapLabel() const { return QString::fromStdString(snap_.label); }
    QString snapKind() const;
    QVector3D snapPoint() const { return QVector3D(float(snap_.point.x), float(snap_.point.y), float(snap_.point.z)); }
    QString hoverObject() const { return QString::fromStdString(snap_.object); }
    QString tool() const { return tool_; }
    QString toolHint() const;
    bool hasBasePoint() const { return base_.has_value(); }
    QPointF basePos() const;
    QVector3D camRight() const;
    QVector3D camUp() const;
    QVector3D camForward() const;
    bool ortho() const { return cam_.ortho; }
    double pixelSizeMm() const { return cam_.world_per_pixel(cam_.distance) * dpr(); }
    bool radialActive() const { return radial_.active(); }
    QPointF radialCenter() const { return {radial_.center_x(), radial_.center_y()}; }
    int radialHighlight() const { return radial_.highlighted(); }
    QVariantList radialPath() const;
    QString radialContext() const { return radial_context_; }
    double frameP50() const { return p50_; }
    double frameP99() const { return p99_; }
    double gpuMs() const { return stats_->gpu_ms; }
    qulonglong triangles() const { return stats_->triangles; }
    qulonglong gpuBytes() const { return stats_->gpu_bytes; }
    qulonglong vramTotal() const { return stats_->device_local_budget; }
    QString deviceName() const { return QString::fromStdString(stats_->device_name); }
    int msaa() const { return stats_->msaa; }

    // --- invokables ----------------------------------------------------------
    Q_INVOKABLE void view(const QString& name);
    Q_INVOKABLE void viewDirection(double x, double y, double z);
    /// ViewCube: (u, v) in [-1,1] on the cube widget (orthographic). Returns one of the
    /// 26 directions (components in {-1,0,1}) or a null vector if the cube was missed.
    Q_INVOKABLE QVector3D cubeDirection(double u, double v) const;
    Q_INVOKABLE void frameAll();
    Q_INVOKABLE void startMoveTool();
    bool sculptMode() const { return tool_ == "sculpt"; }
    /// The evaluated scene has the object's sculpt grid (conversion finished).
    Q_INVOKABLE bool sculptReady(const QString& name) const;
    void setSculptMode(bool on);
    QPointF brushPos() const { return brush_pos_; }
    double brushScreenRadius() const { return brush_px_; }
    QString brushTarget() const { return brush_target_; }
    QStringList sculptObjects() const { return sculpt_objects_; }
    double sculptUpdateMs() const { return sculpt_ms_; }
    /// Called (GUI thread) when a live stroke is complete: object, its root at
    /// stroke start, the resulting grid and the stroke.
    using StrokeDone = std::function<void(const QString&, NodePtr, std::shared_ptr<const sdf::Grid>, sdf::Stroke)>;
    void setStrokeDone(StrokeDone f) { stroke_done_ = std::move(f); }
    Q_INVOKABLE void startDraw(const QString& kind);   // "polyline" | "rect" | "circle"
    Q_INVOKABLE void drawTypedLength(double mm);       // next point at this distance along the cursor direction
    Q_INVOKABLE void drawUndoPoint();
    Q_INVOKABLE void finishDraw();
    Q_INVOKABLE void startPickTools(const QString& target, bool single = false);
    Q_INVOKABLE void finishPick();
    /// World point -> item coordinates (for QML overlays).
    Q_INVOKABLE QPointF drawToItem(double x, double y) const { return sketchToItem(x, y); }
    Q_INVOKABLE QPointF projectWorld(const QVector3D& p) const;
    /// Item point -> sketch-plane coordinates (x, y in mm) for a plane, or null if parallel.
    Q_INVOKABLE QVariant planePoint(double x, double y, const QString& plane, double offset) const;
    Q_INVOKABLE QString pickSketchAt(double x, double y) const;
    Q_INVOKABLE void cancelTool();
    Q_INVOKABLE void radialBegin(QString context);  // at the current pointer position
    Q_INVOKABLE void radialEnd();
    Q_INVOKABLE void radialCancel();
    Q_INVOKABLE QVector3D currentPoint() const { return snapPoint(); }
    /// Benchmark: orbits the camera for `frames` frames and writes JSON to `outPath`.
    Q_INVOKABLE void runBenchmark(int frames, const QString& outPath);

  signals:
    void styleChanged();
    void selectionChanged();
    void snapChanged();
    void toolChanged();
    void cameraChanged();
    void radialChanged();
    void sculptChanged();
    void sculptObjectsChanged();
    /// Pressed on a regular part in sculpt mode: convert it to clay.
    void sculptConvertRequested(const QString& name);
    void brushChanged();
    void statsChanged();
    /// The user picked something (empty name = clicked empty space).
    void picked(const QString& name, bool additive);
    void radialSelected(const QVariantList& path, const QString& context, const QVector3D& point);
    /// Two-point move finished: emit the translation (mm).
    void moveRequested(const QString& name, double dx, double dy, double dz);
    void drawChanged();
    /// Drawing finished: points in sketch-plane coordinates (mm).
    void sketchDrawn(const QString& kind, const QString& plane, double offset, const QVariantList& points, bool closed,
                     const QString& onObject);
    void toolsPicked(const QString& target, const QStringList& tools);
    void objectDoubleClicked(const QString& name);
    void benchmarkFinished(const QString& json);

  protected:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void hoverMoveEvent(QHoverEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void geometryChange(const QRectF& n, const QRectF& o) override;
    void releaseResources() override;
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;

  private slots:
    void handleWindowChanged(QQuickWindow* w);
    void sync();
    void cleanup();
    void frameSwapped();

  private:
    double dpr() const;
    void updateSnap(QPointF pos);
    void animateTo(double yaw, double pitch);
    void requestFrame();

    std::shared_ptr<RendererStats> stats_ = std::make_shared<RendererStats>();
    ViewportRenderer* renderer_ = nullptr;  // render thread
    VkImage shown_image_ = VK_NULL_HANDLE;  // image currently wrapped by the texture node (render thread)
    std::shared_ptr<const Scene> scene_;
    std::shared_ptr<const MeshCpu> ghost_;
    Camera cam_;
    SnapEngine snap_engine_;
    SnapResult snap_;
    bool snap_visible_ = false;
    QStringList selection_;
    QColor bg_{20, 22, 26}, grid_{120, 130, 140}, edge_{0, 0, 0, 140};
    QVariantList object_colors_;
    bool show_grid_ = true;
    QColor wire_{0, 0, 0, 90};
    bool mesh_view_ = false;
    QString scenery_ = "none";

    // interaction
    QPointF press_pos_, last_pos_, hover_pos_;
    Qt::MouseButton drag_button_ = Qt::NoButton;
    bool dragging_ = false;
    QString tool_ = "select";
    // drawing
    struct Draw {
        QString kind;
        bool have_plane = false;
        SketchPlane plane = SketchPlane::XY;
        double offset = 0;
        Vec3 o, u, v, n;
        std::vector<std::array<double, 2>> pts;
        std::array<double, 2> cursor{0, 0};
        bool cursor_valid = false;
        QString guide;
        QString on;
    } draw_;
    QStringList picked_tools_;
    QString pick_target_;
    void drawMove(QPointF pos);
    void drawClick(bool double_click);
    void setPlaneFrame(SketchPlane p, double offset);
    std::optional<std::array<double, 2>> rayToPlane(QPointF pos) const;
    QPointF sketchToItem(double x, double y) const;
    std::optional<Vec3> base_;
    RadialGesture radial_;
    RadialRingProvider radial_rings_;
    std::function<QString(const QString&)> context_resolver_;
    // sculpting
    QString sculpt_brush_ = "draw";
    double sculpt_radius_ = 2.0, sculpt_strength_ = 0.7, sculpt_falloff_ = 1.0;
    bool sculpt_invert_ = false;
    QString sculpt_pattern_ = "scales";
    double sculpt_pattern_size_ = 0.6;
    QString sculpt_symmetry_;
    QStringList sculpt_objects_;          // objects whose clay is evaluated (strokes possible)
    sdf::Brush stroke_brush_ = sdf::Brush::Draw;
    NodePtr expected_root_;               // object root after the last finished stroke
    QPointF brush_pos_{-1e6, -1e6};
    double brush_px_ = 0, sculpt_ms_ = 0;
    QString brush_target_;
    bool sculpting_ = false;
    double stroke_t_ = 0;                 // ray depth of the last stroke point
    Vec3 grab_plane_p_, grab_plane_n_;
    std::unique_ptr<class SculptEngine> sculpt_engine_;
    StrokeDone stroke_done_;
    NodePtr stroke_root_;                 // object root when the stroke began
    std::shared_ptr<const sdf::Grid> pending_grid_;  // finished stroke, until the scene catches up
    const SceneObject* sculpt_object_at(QPointF pos, double* t_hit, Vec3* p_hit) const;
    std::shared_ptr<const sdf::Grid> sculpt_grid_of(const SceneObject& o) const;
    bool sculptPress(QMouseEvent* e);
    bool sculptMove(QPointF pos);
    void sculptRelease();
    void sculptHover(QPointF pos);
    // live sculpt stroke: replacement meshes per chunk of one object
    struct LiveParts {
        std::string object;
        std::unordered_map<std::uint64_t, std::shared_ptr<const MeshCpu>> chunks;
        bool done = false;
    } live_;
    static std::uint64_t sdf_chunk_key(const sdf::IVec3& c) {
        return (std::uint64_t(std::uint32_t(c.x)) << 42) ^ (std::uint64_t(std::uint32_t(c.y)) << 21) ^ std::uint32_t(c.z);
    }
    bool pick_single_ = false;
    QString radial_context_;
    QVector3D radial_point_;
    Vec3 radial_point_exact_{};
    bool radial_by_mouse_ = false;

    // camera animation
    bool animating_ = false;
    double anim_from_[2]{}, anim_to_[2]{};
    QElapsedTimer anim_clock_;

    // frame statistics (present-to-present intervals on the GUI thread)
    QElapsedTimer frame_clock_;
    std::vector<double> frame_ms_;
    std::size_t frame_idx_ = 0;
    double p50_ = 0, p99_ = 0;
    int stats_tick_ = 0;
    // benchmark
    int bench_left_ = 0, bench_total_ = 0;
    QString bench_out_;
    std::vector<double> bench_ms_, bench_gpu_;
    double bench_yaw0_ = 0;
};

}  // namespace anvil
