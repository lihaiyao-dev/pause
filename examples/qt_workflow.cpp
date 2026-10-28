// Bound clice's preamble before template-heavy includes to avoid PCH reload bugs.
// This boundary also covers the embedded acceptance routines below.
namespace { class Window; }

#include "qt_workflow.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QGraphicsItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string_view>

namespace {
using namespace pause_graph;
QString status_text(NodeStatus s)
{
    switch (s) {
    case NodeStatus::Pending: return QStringLiteral("未开始");
    case NodeStatus::Waiting: return QStringLiteral("等待安全点放行");
    case NodeStatus::Running: return QStringLiteral("运行中");
    case NodeStatus::Joining: return QStringLiteral("等待子路径汇合");
    case NodeStatus::Completed: return QStringLiteral("已完成");
    case NodeStatus::Skipped: return QStringLiteral("条件跳过");
    case NodeStatus::Cancelled: return QStringLiteral("已取消");
    case NodeStatus::Stopped: return QStringLiteral("返回 stopped");
    case NodeStatus::Failed: return QStringLiteral("异常");
    case NodeStatus::Abandoned: return QStringLiteral("上游结束，未执行");
    }
    return {};
}
QColor status_color(NodeStatus s)
{
    switch (s) {
    case NodeStatus::Waiting: return QColor("#d97706");
    case NodeStatus::Running: return QColor("#2563eb");
    case NodeStatus::Joining: return QColor("#7c3aed");
    case NodeStatus::Completed: return QColor("#059669");
    case NodeStatus::Cancelled: case NodeStatus::Stopped: case NodeStatus::Failed: return QColor("#dc2626");
    default: return QColor("#94a3b8");
    }
}
QString kind_text(NodeKind kind)
{
    switch (kind) {
    case NodeKind::Work: return QStringLiteral("步骤");
    case NodeKind::Fork: return QStringLiteral("分叉");
    case NodeKind::Join: return QStringLiteral("汇合");
    case NodeKind::Decision: return QStringLiteral("条件");
    case NodeKind::Controller: return QStringLiteral("控制");
    }
    return {};
}
double fraction(const NodeProgress& node)
{
    if (node.status == NodeStatus::Completed || node.status == NodeStatus::Skipped) return 1;
    if (node.duration_ms == 0) return 0;
    return std::clamp(double(node.elapsed.count()) / node.duration_ms, 0.0, 0.99);
}
class NodeItem final : public QGraphicsItem {
    int id_;
    NodeSpec spec_;
    NodeProgress progress_;
public:
    NodeItem(int id, const NodeSpec& spec) : id_(id), spec_(spec)
    {
        setPos(25 + spec.column * 215, 25 + spec.row * 118);
        setFlag(ItemIsSelectable);
    }
    int id() const { return id_; }
    QRectF boundingRect() const override { return {-3, -3, 186, 96}; }
    void show_progress(const NodeProgress& p)
    {
        progress_ = p;
        setToolTip(QString::fromStdString(spec_.title + "\n" + spec_.key + "\n" + p.note));
        update();
    }
    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override
    {
        const auto color = status_color(progress_.status);
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(isSelected() ? QColor("#0f172a") : color, isSelected() ? 3 : 1.5,
            spec_.kind == NodeKind::Work ? Qt::SolidLine : Qt::DashLine));
        painter->setBrush(Qt::white);
        painter->drawRoundedRect(QRectF(0, 0, 180, 90), 8, 8);
        QFont font = painter->font();
        font.setPixelSize(14); font.setBold(true); painter->setFont(font);
        painter->setPen(QColor("#0f172a"));
        painter->drawText(QRectF(10, 6, 160, 21), Qt::AlignLeft | Qt::AlignVCenter,
            painter->fontMetrics().elidedText(QString::fromStdString(spec_.title), Qt::ElideRight, 160));
        font.setPixelSize(11); font.setBold(false); painter->setFont(font);
        painter->setPen(color);
        painter->drawText(QRectF(10, 29, 160, 18), status_text(progress_.status));
        if (progress_.duration_ms != 0) {
            painter->setPen(Qt::NoPen); painter->setBrush(QColor("#e2e8f0"));
            painter->drawRoundedRect(QRectF(10, 54, 160, 6), 3, 3);
            painter->setBrush(color);
            painter->drawRoundedRect(QRectF(10, 54, 160 * fraction(progress_), 6), 3, 3);
            painter->setPen(QColor("#475569"));
            painter->drawText(QRectF(10, 66, 164, 18), QStringLiteral("%1 / %2 s   %3%")
                .arg(progress_.elapsed.count() / 1000.0, 0, 'f', 2)
                .arg(progress_.duration_ms / 1000.0, 0, 'f', 2).arg(int(fraction(progress_) * 100)));
        } else {
            painter->setPen(QColor("#475569"));
            painter->drawText(QRectF(10, 52, 160, 18), spec_.kind == NodeKind::Work
                ? QStringLiteral("未指定预计时长") : kind_text(spec_.kind));
            painter->drawText(QRectF(10, 68, 160, 16),
                (spec_.kind == NodeKind::Work ? QStringLiteral("实际耗时 %1 s") : QStringLiteral("等待 / 活跃时间 %1 s"))
                    .arg(progress_.elapsed.count() / 1000.0, 0, 'f', 2));
        }
    }
};
class GraphView final : public QGraphicsView {
    QGraphicsScene scene_;
    std::vector<NodeItem*> nodes_;
    std::vector<QGraphicsPathItem*> edges_;
    std::vector<Edge> descriptions_;
    bool fit_on_resize_ = true;
public:
    std::function<void(int)> selected = [](int) {};
    explicit GraphView(QWidget* parent = nullptr) : QGraphicsView(parent)
    {
        setScene(&scene_); setRenderHint(QPainter::Antialiasing);
        setBackgroundBrush(QColor("#f8fafc")); setFrameShape(QFrame::NoFrame);
        setDragMode(ScrollHandDrag);
        setTransformationAnchor(AnchorUnderMouse);
        connect(&scene_, &QGraphicsScene::selectionChanged, this, [this] {
            for (auto* item : scene_.selectedItems())
                if (auto* node = dynamic_cast<NodeItem*>(item)) { selected(node->id()); break; }
        });
    }
    void set_graph(const Graph& graph)
    {
        scene_.clear(); nodes_.clear(); edges_.clear(); descriptions_ = graph.edges;
        scene_.setSceneRect(0, 0, graph.columns * 215 + 30, graph.rows * 118 + 40);
        for (int i = 0; i < static_cast<int>(graph.nodes.size()); ++i) {
            auto* item = new NodeItem(i, graph.nodes[i]); scene_.addItem(item); nodes_.push_back(item);
        }
        for (const auto& edge : graph.edges) {
            const auto start = nodes_[edge.from]->pos() + QPointF(183, 45);
            const auto end = nodes_[edge.to]->pos() + QPointF(-3, 45);
            QPainterPath path(start);
            const auto distance = std::min(60.0, (end.x() - start.x()) / 2);
            if (edge.label == "跳过") {
                // Route the bypass below its work card, rather than through it.
                path.cubicTo(start + QPointF(12, 0), start + QPointF(12, 70), start + QPointF(20, 70));
                path.lineTo(end + QPointF(-20, 70));
                path.cubicTo(end + QPointF(-12, 70), end + QPointF(-12, 0), end);
            } else path.cubicTo(start + QPointF(distance, 0), end - QPointF(distance, 0), end);
            const auto label_point = edge.label == "跳过" ? (start + end) / 2 + QPointF(0, 52)
                : path.pointAtPercent(0.5) + QPointF(0, -22);
            path.moveTo(end - QPointF(7, 4)); path.lineTo(end); path.lineTo(end - QPointF(7, -4));
            auto* item = scene_.addPath(path, QPen(QColor("#cbd5e1"), 2));
            item->setZValue(-1); item->setToolTip(QString::fromStdString(edge.label)); edges_.push_back(item);
            if (!edge.label.empty() && edge.label != "0 个任务") {
                auto* label = scene_.addText(QString::fromStdString(edge.label));
                QFont font = label->font(); font.setPixelSize(11); label->setFont(font);
                label->setDefaultTextColor(edge.enabled ? QColor("#475569") : QColor("#94a3b8"));
                label->setPos(label_point - QPointF(label->boundingRect().width() / 2, 0));
            }
        }
        readable(QPointF(0, 0));
    }
    void fit_graph() { fit_on_resize_ = true; fitInView(scene_.sceneRect(), Qt::KeepAspectRatio); }
    void readable(QPointF center)
    {
        fit_on_resize_ = false; resetTransform(); scale(0.85, 0.85); centerOn(center);
    }
    void readable_node(int id) { readable(nodes_[id]->pos() + QPointF(90, 45)); }
    void select(int id) { scene_.clearSelection(); nodes_[id]->setSelected(true); }
    void show_progress(const Progress& p)
    {
        for (int i = 0; i < static_cast<int>(nodes_.size()); ++i) nodes_[i]->show_progress(p.nodes[i]);
        for (int i = 0; i < static_cast<int>(edges_.size()); ++i) {
            const auto& edge = descriptions_[i];
            const auto from = p.nodes[edge.from].status;
            auto status = p.nodes[edge.to].status;
            if (status == NodeStatus::Pending) status = from;
            const bool inactive = !edge.enabled || from == NodeStatus::Skipped || from == NodeStatus::Abandoned
                || status == NodeStatus::Skipped || status == NodeStatus::Abandoned;
            if (inactive) status = NodeStatus::Skipped;
            else if (from == NodeStatus::Cancelled || from == NodeStatus::Stopped || from == NodeStatus::Failed) status = from;
            edges_[i]->setPen(QPen(status_color(status), 2, inactive ? Qt::DashLine : Qt::SolidLine));
        }
    }
    bool save_graph(const QString& path)
    {
        QImage image(scene_.sceneRect().size().toSize(), QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor("#f8fafc")); QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing); scene_.render(&painter);
        return image.save(path);
    }
protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QGraphicsView::resizeEvent(e);
        if (fit_on_resize_) fit_graph();
    }
    void wheelEvent(QWheelEvent* e) override
    {
        fit_on_resize_ = false;
        const double next = std::clamp(transform().m11() * std::pow(1.0015, e->angleDelta().y()), 0.15, 2.0);
        scale(next / transform().m11(), next / transform().m11()); e->accept();
    }
};
// Both views show the same scene: the overview keeps the complete graph
// visible while the main view preserves readable node labels and timings.
class Overview final : public QGraphicsView {
    GraphView& graph_;
public:
    Overview(GraphView& graph, QWidget* parent) : QGraphicsView(graph.scene(), parent), graph_(graph)
    {
        setFixedHeight(140); setRenderHint(QPainter::Antialiasing);
        setBackgroundBrush(QColor("#f8fafc")); setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setToolTip(QStringLiteral("全图概览：点击位置，在下方查看该区域"));
        connect(scene(), &QGraphicsScene::sceneRectChanged, this,
            [this](const QRectF& rect) { fitInView(rect, Qt::KeepAspectRatio); });
    }
protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QGraphicsView::resizeEvent(e); fitInView(scene()->sceneRect(), Qt::KeepAspectRatio);
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        graph_.readable(mapToScene(e->position().toPoint())); e->accept();
    }
};
class Window final : public QWidget {
    std::unique_ptr<Session> session_;
    Workflow workflow_;
    Options options_;
    GraphView* graph_ = new GraphView(this);
    QLabel* state_ = new QLabel(this);
    QLabel* timing_ = new QLabel(this);
    QLabel* details_ = new QLabel(this);
    QLabel* command_ = new QLabel(this);
    QProgressBar* total_ = new QProgressBar(this);
    QTimer refresh_;
    enum class PendingAction { None, Restart, Close };
    PendingAction pending_ = PendingAction::None;
    int selected_ = 0;
    bool timing_active_ = false;
    Clock::time_point advanced_at_{};
    Milliseconds advance_elapsed_{0};

    void start()
    {
        pending_ = PendingAction::None;
        timing_active_ = false; advance_elapsed_ = Milliseconds{0};
        // Restart reaches here after completion; release the old host first.
        session_.reset();
        session_ = std::make_unique<Session>(workflow_, options_);
        graph_->set_graph(session_->graph()); selected_ = 0; graph_->select(selected_);
        command_->setText(QStringLiteral("入口暂停。点击节点查看状态，滚轮缩放，拖动平移。"));
        session_->start();
    }
    void advance(AdvanceMode mode)
    {
        const auto result = session_->advance(mode);
        if (!result) command_->setText(QStringLiteral("控制命令失败（PauseError=%1）").arg(int(result.error())));
        else {
            advanced_at_ = Clock::now(); advance_elapsed_ = Milliseconds{0}; timing_active_ = true;
            command_->setText(mode == AdvanceMode::SingleStep ? QStringLiteral("本轮放行，到下一安全点再次暂停。")
                : QStringLiteral("各路径分别执行；汇合等待所有路径结束。"));
        }
        refresh();
    }
public:
    QPushButton* pause_button = new QPushButton(QStringLiteral("暂停"), this);
    QPushButton* continue_button = new QPushButton(QStringLiteral("继续"), this);
    QPushButton* step_button = new QPushButton(QStringLiteral("单步"), this);
    QPushButton* cancel_button = new QPushButton(QStringLiteral("取消全部"), this);
    QPushButton* restart_button = new QPushButton(QStringLiteral("重新开始"), this);

    explicit Window(Workflow workflow, Options options = {}) : workflow_(std::move(workflow)), options_(options)
    {
        setWindowTitle(QStringLiteral("Pause · sender 分支图")); resize(1560, 980); setMinimumSize(1000, 700);
        setStyleSheet(QStringLiteral("QWidget { font-family: 'Microsoft YaHei', sans-serif; font-size: 13px; } QPushButton { padding: 6px 12px; }"));
        auto* layout = new QVBoxLayout(this); layout->setContentsMargins(16, 12, 16, 12);
        auto* title = new QLabel(QStringLiteral("业务运行状态"), this);
        title->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: bold;")); layout->addWidget(title);
        auto* note = new QLabel(QStringLiteral("暂停在安全点生效，单步放行所有等待分支。预览步骤随机耗时 %1–%2 秒；接入实际业务时显示实际耗时。")
            .arg(options.min_ms / 1000.0).arg(options.max_ms / 1000.0), this);
        note->setWordWrap(true); layout->addWidget(note);
        auto* controls = new QHBoxLayout;
        for (auto* button : {pause_button, continue_button, step_button, cancel_button, restart_button})
            controls->addWidget(button);
        auto* fit = new QPushButton(QStringLiteral("全图"), this); controls->addWidget(fit);
        auto* readable = new QPushButton(QStringLiteral("放大选中节点"), this); controls->addWidget(readable); controls->addStretch();
        layout->addLayout(controls); layout->addWidget(state_); layout->addWidget(timing_); layout->addWidget(total_);
        layout->addWidget(new Overview(*graph_, this)); layout->addWidget(graph_, 1);
        details_->setWordWrap(true); details_->setMinimumHeight(52); layout->addWidget(details_);
        layout->addWidget(new QLabel(QStringLiteral("橙：安全点等待  蓝：运行  紫：汇合等待  绿：完成  灰：跳过/未执行  红：停止/取消/异常"), this));
        command_->setWordWrap(true); layout->addWidget(command_);
        graph_->selected = [this](int id) { selected_ = id; if (session_) refresh(); };
        connect(fit, &QPushButton::clicked, this, [this] { graph_->fit_graph(); });
        connect(readable, &QPushButton::clicked, this, [this] { graph_->readable_node(selected_); });
        connect(step_button, &QPushButton::clicked, this, [this] { advance(AdvanceMode::SingleStep); });
        connect(continue_button, &QPushButton::clicked, this, [this] { advance(AdvanceMode::Continuous); });
        connect(pause_button, &QPushButton::clicked, this, [this] {
            const auto result = session_->pause();
            command_->setText(result ? QStringLiteral("已请求暂停，等待正在执行的业务到达安全点。")
                : QStringLiteral("暂停失败（PauseError=%1）").arg(int(result.error())));
            refresh();
        });
        connect(cancel_button, &QPushButton::clicked, this, [this] { session_->cancel(); });
        connect(restart_button, &QPushButton::clicked, this, [this] {
            pending_ = PendingAction::Restart; session_->cancel(); refresh();
        });
        connect(&refresh_, &QTimer::timeout, this, [this] { refresh(); });
        start(); refresh_.start(33); refresh();
    }
    Session& session() { return *session_; }
    void select(const std::string& key) { graph_->select(session_->graph().find(key)); }
    bool save_graph(const QString& path) { return graph_->save_graph(path); }
    void refresh()
    {
        if (!session_) return;
        // Read the PauseSource first so converged arrivals precede the UI copy.
        auto pause = session_->snapshot();
        auto progress = session_->progress();
        if (pending_ == PendingAction::Restart && progress.completion != Completion::Active) {
            start(); pause = session_->snapshot(); progress = session_->progress();
        }
        update_controls(pause, progress);
        update_summary(pause, progress);
        update_progress(progress);
        if (pending_ == PendingAction::Close && progress.completion != Completion::Active) close();
    }
private:
    void update_controls(const PauseSnapshot& pause, const Progress& progress)
    {
        const bool available = progress.completion == Completion::Active && pending_ == PendingAction::None;
        const bool paused = pause.phase == GatePhase::Paused && pause.waiting_branch_count != 0;
        pause_button->setEnabled(available && pause.root_created && pause.phase == GatePhase::Open);
        continue_button->setEnabled(available && paused); step_button->setEnabled(available && paused);
        cancel_button->setEnabled(available);
        restart_button->setEnabled(pending_ == PendingAction::None);
    }
    void update_summary(const PauseSnapshot& pause, const Progress& progress)
    {
        QString state;
        if (pending_ != PendingAction::None) state = QStringLiteral("正在取消并等待全部任务退出…");
        else if (progress.completion == Completion::Value) state = QStringLiteral("工作流完成");
        else if (progress.completion == Completion::Stopped) state = QStringLiteral("整个工作流 stopped");
        else if (progress.completion == Completion::Error) state = QStringLiteral("工作流异常：%1").arg(QString::fromStdString(progress.error));
        else if (!pause.root_created) state = QStringLiteral("初始化中");
        else if (pause.phase == GatePhase::Open) state = QStringLiteral("连续运行");
        else if (pause.phase == GatePhase::Collecting) state = QStringLiteral("单步运行 / 收集暂停责任");
        else state = QStringLiteral("已暂停");
        state_->setText(QStringLiteral("%1  |  有效暂停责任 %2  ·  等待分支 %3  ·  单步轮次 %4")
            .arg(state).arg(qulonglong(pause.live_branch_count)).arg(qulonglong(pause.waiting_branch_count))
            .arg(pause.step_round ? QString::number(qulonglong(*pause.step_round)) : QStringLiteral("—")));
        if (timing_active_) {
            advance_elapsed_ = std::chrono::duration_cast<Milliseconds>(Clock::now() - advanced_at_);
            if (progress.completion != Completion::Active || (pause.phase == GatePhase::Paused && pause.waiting_branch_count))
                timing_active_ = false;
        }
        timing_->setText(QStringLiteral("本次放行耗时：%1 s%2").arg(advance_elapsed_.count() / 1000.0, 0, 'f', 2)
            .arg(timing_active_ ? QStringLiteral("（计时中）") : QStringLiteral("（不含本轮暂停等待）")));
    }
    void update_progress(const Progress& progress)
    {
        const auto& graph = session_->graph();
        double units = 0; int completed = 0, work_count = 0, skipped = 0;
        for (int i = 0; i < static_cast<int>(graph.nodes.size()); ++i) {
            if (graph.nodes[i].kind != NodeKind::Work) continue;
            ++work_count; units += fraction(progress.nodes[i]);
            completed += progress.nodes[i].status == NodeStatus::Completed;
            skipped += progress.nodes[i].status == NodeStatus::Skipped;
        }
        total_->setRange(0, std::max(1, work_count) * 100);
        total_->setValue(work_count ? int(units * 100) : progress.completion == Completion::Value ? 100 : 0);
        total_->setFormat(QStringLiteral("业务步骤 %1 / %2 完成 · %3 跳过 · 步骤进度 %p%").arg(completed).arg(work_count).arg(skipped));
        graph_->show_progress(progress);
        const auto& spec = graph.nodes[selected_]; const auto& selected = progress.nodes[selected_];
        details_->setText(QStringLiteral("选中：%1  [%2]  ·  %3  ·  %4\n已用 %5 s%6  %7")
            .arg(QString::fromStdString(spec.title), QString::fromStdString(spec.key), kind_text(spec.kind), status_text(selected.status))
            .arg(selected.elapsed.count() / 1000.0, 0, 'f', 3)
            .arg(selected.duration_ms ? QStringLiteral(" / 计划 %1 s").arg(selected.duration_ms / 1000.0, 0, 'f', 3)
                : spec.kind == NodeKind::Work ? QStringLiteral("（未指定预计时长，显示实际耗时）")
                : QStringLiteral("（结构节点；汇合等待包含子路径暂停）"))
            .arg(QString::fromStdString(selected.note)));
    }
protected:
    void closeEvent(QCloseEvent* e) override
    {
        if (session_->progress().completion == Completion::Active) {
            pending_ = PendingAction::Close; session_->cancel(); e->ignore();
        } else e->accept();
    }
};
} // namespace
// Offscreen acceptance uses the UI types above; keep it in this translation unit.
// Acceptance focuses on UI controls and caller-supplied senders.
namespace {
inline bool valid_smoke(std::string_view name)
{
    constexpr std::string_view names[]{"controls", "single-step", "close-paused", "close-running", "cancel-all",
        "cancel-running", "restart-active", "restart-paused", "sender-values", "sender-error", "sender-stopped",
        "sender-async", "custom-workflow", "prestop", "empty-group"};
    return std::find(std::begin(names), std::end(names), name) != std::end(names);
}
inline Options smoke_options(std::string_view name)
{
    return {.min_ms = 80, .max_ms = 240, .pre_stopped = name == "prestop"};
}
inline stdexec::task<int> value_leaf(RunContext& ui, Branch branch, std::string key, int value)
{
    co_return co_await (ui.step(branch, key, stdexec::just(std::make_unique<int>(value), std::string("payload")))
        | stdexec::then([](std::unique_ptr<int> result, std::string label) {
            require(label == "payload", "sender lost an extra value argument");
            return *result;
        }));
}
inline Workflow smoke_workflow(std::string_view name)
{
    if (name == "sender-async") {
        return {sequence({step("async", "真实异步 sender"), step("after", "结果处理")}),
            [](RunContext& ui, Branch& root) -> stdexec::task<void> {
                auto operation = ui.step(root, "async", exec::schedule_after(ui.timer(), Milliseconds{100})
                    | stdexec::then([] { return std::make_unique<int>(42); }), 100)
                    | stdexec::let_value([&](auto& value) {
                        require(*value == 42, "async sender lost its result");
                        return ui.step(root, "after", stdexec::just());
                    });
                co_await std::move(operation);
            }};
    }
    if (name == "sender-values") {
        return {parallel("pack", "用户值组合", {step("left", "左值"), step("right", "右值")}),
            [](RunContext& ui, Branch& root) -> stdexec::task<void> {
                co_await ui.group(root, "pack", 2, [&](auto& paths) {
                    return stdexec::when_all(value_leaf(ui, std::move(paths[0]), "left", 7),
                        value_leaf(ui, std::move(paths[1]), "right", 9))
                        | stdexec::then([](int a, int b) { require(a + b == 16, "when_all lost values"); });
                });
            }};
    }
    if (name == "sender-error" || name == "sender-stopped") {
        const bool fail = name == "sender-error";
        return {sequence({step("fault", "用户完成通道"), step("after", "下游")}),
            [fail](RunContext& ui, Branch& root) -> stdexec::task<void> {
                if (fail) co_await ui.step(root, "fault", stdexec::just_error(std::runtime_error("user failure")));
                else co_await ui.step(root, "fault", stdexec::just_stopped());
                co_await ui.delay_step(root, "after");
            }};
    }
    if (name == "custom-workflow") {
        return {sequence({step("alpha", "自定义入口"), step("beta", "自定义后续")}),
            [](RunContext& ui, Branch& root) -> stdexec::task<void> {
                co_await (ui.step(root, "alpha", stdexec::just(21))
                    | stdexec::let_value([&](int value) {
                        return ui.step(root, "beta", stdexec::just(value * 2));
                    }) | stdexec::then([](int value) { require(value == 42, "user composition changed"); }));
            }};
    }
    if (name == "empty-group") {
        return {sequence({parallel("empty", "空组合", {}), step("after", "后续")}),
            [](RunContext& ui, Branch& root) -> stdexec::task<void> {
                co_await ui.group(root, "empty", 0, [](auto&) { return stdexec::when_all(); });
                co_await ui.delay_step(root, "after");
            }};
    }
    return make_workflow();
}
struct SmokeState {
    int stage = 0;
    Clock::time_point started = Clock::now(), since = started;
    std::optional<coop_pause::PauseEpoch> old_epoch;
    bool saw_short_wait = false, saw_pause = false, finished = false, failed = false;
};
inline void smoke_tick(Window& window, std::string_view name, SmokeState& s)
{
    window.refresh();
    const auto gate = window.session().snapshot();
    const auto p = window.session().progress();
    const auto& graph = window.session().graph();
    const bool paused = gate.phase == GatePhase::Paused && gate.waiting_branch_count != 0;
    const auto node = [&](const char* key) -> const NodeProgress& { return p.nodes[graph.find(key)]; };
    const auto completed = [&] {
        return std::count_if(p.nodes.begin(), p.nodes.end(), [](const auto& n) { return n.status == NodeStatus::Completed; });
    };
    const auto elapsed = std::chrono::duration_cast<Milliseconds>(Clock::now() - s.since).count();
    if (s.stage == 99) return; // Async window close is validated after app.exec().
    if (s.stage == 9 && paused) {
        require(completed() == 0 && node("input").status == NodeStatus::Waiting, "restart retained old state");
        s.finished = true; s.stage = 99; window.close(); return;
    }
    if (p.completion != Completion::Active) {
        require_drained(window.session().source());
        const bool cancelled = name == "prestop" || name == "cancel-all" || name == "cancel-running";
        require(p.completion == (name == "sender-error" ? Completion::Error
            : cancelled || name == "sender-stopped" ? Completion::Stopped : Completion::Value), "wrong completion");
        if (name == "sender-error" || name == "sender-stopped") {
            require(node("fault").status == (name == "sender-error" ? NodeStatus::Failed : NodeStatus::Stopped), "lost completion status");
            require(node("after").status == NodeStatus::Abandoned, "downstream ran after error/stopped");
        } else if (name == "sender-async") {
            require(node("async").elapsed.count() >= 80 && node("async").duration_ms == 100,
                "async sender did not report real timing");
            require(node("after").duration_ms == 0, "real sender received a simulated duration");
        } else if (!cancelled) {
            for (const auto& n : p.nodes) require(n.status == NodeStatus::Completed, "normal graph has an unfinished node");
        }
        if (name == "controls") {
            require(s.saw_short_wait && s.saw_pause, "did not exercise branch join and pause");
            s.stage = 9; window.restart_button->click(); return;
        }
        s.finished = true; window.close(); return;
    }
    if (s.stage == 0 && paused) {
        require(completed() == 0, "entry ran before resume");
        s.stage = 1; s.since = Clock::now();
    } else if (s.stage == 1 && elapsed >= 80 && window.step_button->isEnabled()) {
        if (name == "close-paused") { s.finished = true; s.stage = 99; window.close(); return; }
        if (name == "restart-paused") { s.stage = 9; window.restart_button->click(); return; }
        if (name.starts_with("sender-") || name == "custom-workflow" || name == "empty-group") {
            window.continue_button->click(); s.stage = 8; return;
        }
        window.step_button->click(); s.stage = 2;
    } else if (s.stage == 2 && node("input").status == NodeStatus::Running && node("input").elapsed.count() > 20) {
        require(fraction(node("input")) > 0 && fraction(node("input")) < 1, "no time progress");
        if (name == "close-running") { s.finished = true; s.stage = 99; window.close(); return; }
        if (name == "restart-active") { s.stage = 9; window.restart_button->click(); return; }
        if (name == "cancel-running") { window.cancel_button->click(); s.stage = 8; return; }
        s.stage = 3;
    } else if (s.stage == 3 && paused && window.step_button->isEnabled()) {
        require(node("input").status == NodeStatus::Completed && node("outer.fork").status == NodeStatus::Waiting, "single step crossed fork");
        window.step_button->click(); s.stage = 4;
    } else if (s.stage == 4 && paused && window.step_button->isEnabled()) {
        require(gate.waiting_branch_count == 2 && node("outer.join").status == NodeStatus::Joining, "fork did not expose two paths");
        window.step_button->click(); s.stage = 5;
    } else if (s.stage == 5 && paused && window.continue_button->isEnabled()) {
        require(node("short").status == NodeStatus::Completed && node("long-end").status == NodeStatus::Waiting
            && node("outer.join").status == NodeStatus::Joining, "short completion released the long path");
        // Keep this barrier for one UI tick so screenshot export cannot delay
        // observation of the following running step.
        if (!s.saw_short_wait) { s.saw_short_wait = true; return; }
        s.old_epoch = gate.paused_epoch;
        if (name == "cancel-all") { window.cancel_button->click(); s.stage = 8; return; }
        if (name == "single-step") { window.step_button->click(); s.stage = 8; return; }
        window.continue_button->click(); s.stage = 6;
    } else if (s.stage == 6 && window.pause_button->isEnabled() && node("long-end").status == NodeStatus::Running) {
        window.pause_button->click(); s.stage = 7;
    } else if (s.stage == 7 && paused) {
        const auto stale = window.session().source().resume(*s.old_epoch, AdvanceMode::Continuous);
        require(!stale && stale.error() == PauseError::StalePauseEpoch, "stale epoch accepted");
        s.saw_pause = true; s.since = Clock::now(); s.stage = 10;
    } else if (s.stage == 10 && elapsed >= 80 && window.continue_button->isEnabled()) {
        require(paused, "pause did not hold");
        window.continue_button->click(); s.stage = 8;
    } else if (s.stage == 8 && name == "single-step" && paused && window.step_button->isEnabled()) {
        window.step_button->click();
    }
}
} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    std::string_view scenario;
    QString screenshot;
    if (argc != 1) {
        if ((argc == 3 || argc == 5) && std::string_view(argv[1]) == "--smoke-test") {
            scenario = argv[2];
            if (argc == 5 && std::string_view(argv[3]) == "--screenshot") screenshot = QString::fromLocal8Bit(argv[4]);
            else if (argc == 5) return 2;
        }
        if (!valid_smoke(scenario)) { std::fputs("usage: pause_qt_workflow [--smoke-test scenario [--screenshot file.png]]\n", stderr); return 2; }
    }
    Window window(scenario.empty() ? make_workflow() : smoke_workflow(scenario),
                  scenario.empty() ? Options{} : smoke_options(scenario));
    window.show();
    QTimer smoke;
    SmokeState state;
    if (!scenario.empty()) {
        QObject::connect(&smoke, &QTimer::timeout, &window, [&] {
            try {
                require(Clock::now() - state.started < std::chrono::seconds(40), "Qt smoke test timed out");
                smoke_tick(window, scenario, state);
                if (!screenshot.isEmpty() && state.saw_short_wait) {
                    window.refresh();
                    require(window.grab().save(screenshot), "could not save UI screenshot");
                    require(window.save_graph(screenshot + ".graph.png"), "could not save graph image");
                    screenshot.clear();
                }
            } catch (const std::exception& error) {
                std::fprintf(stderr, "Qt smoke (%.*s, stage %d): %s\n", int(scenario.size()), scenario.data(), state.stage, error.what());
                state.failed = true; smoke.stop(); window.close();
            }
        });
        smoke.start(10);
    }
    const int result = app.exec();
    if (!scenario.empty() && !state.failed) {
        try {
            require(state.finished, "window closed before smoke scenario completed");
            if (state.stage == 99)
                require(window.session().progress().completion == Completion::Stopped, "close/restart did not cancel workflow");
            require_drained(window.session().source());
            std::printf("PASS Qt %.*s\n", int(scenario.size()), scenario.data());
        } catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); state.failed = true; }
    }
    return state.failed ? 1 : result;
}
