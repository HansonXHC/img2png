#include "mainwindow.h"
#include "convert.h"
#include "enc/pngenc.h"

#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QThreadPool>
#include <QVBoxLayout>

/* ---------------- DropPathEdit ---------------- */

DropPathEdit::DropPathEdit(QWidget *parent)
    : QLineEdit(parent)
{
    setAcceptDrops(true);
    setPlaceholderText("可拖拽文件夹到此框作为输出目录");
}

void DropPathEdit::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        event->acceptProposedAction();
        return;
    }
    event->ignore();
}

void DropPathEdit::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls) {
        QString local = url.toLocalFile();
        QFileInfo fi(local);
        if (fi.isDir()) {
            setText(QDir::toNativeSeparators(local));
            event->acceptProposedAction();
            return;
        }
        if (fi.isFile()) {
            setText(QDir::toNativeSeparators(fi.absolutePath()));
            event->acceptProposedAction();
            return;
        }
    }
    event->ignore();
}

/* ---------------- ConvertTask ---------------- */

ConvertTask::ConvertTask(MainWindow *win, QString in, QString out,
                         int level, int filter, bool autoOpt, bool keepTime,
                         bool replaceOriginal)
    : win_(win), in_(std::move(in)), out_(std::move(out)),
      level_(level), filter_(filter), autoOpt_(autoOpt), keepTime_(keepTime),
      replace_(replaceOriginal)
{
}

void ConvertTask::run()
{
    png_opts_t opts;
    opts.level = level_;
    opts.filter = (png_filter_mode_t)filter_;
    opts.auto_optimize = autoOpt_ ? 1 : 0;
    opts.recompress_png = 1;

    img2png_result_t r;
    img2png_convert(in_.toLocal8Bit().constData(), out_.toLocal8Bit().constData(),
                    &opts, keepTime_ ? 1 : 0, &r);

    QString line;
    if (r.ok) {
        /* "覆盖原文件" on: a non-PNG source whose .png landed in the same
         * folder is replaced by it (original deleted after success) */
        bool replaced = false;
        if (replace_ && !in_.endsWith(".png", Qt::CaseInsensitive) &&
            QFileInfo(out_).absolutePath() == QFileInfo(in_).absolutePath()) {
            replaced = QFile::remove(in_);
        }
        line = QString("[完成] %1 (%2 %3位 %4x%5) -> %6  %7 -> %8 字节, %9s%10")
                   .arg(in_, img2png_color_name(r.out_color))
                   .arg(r.out_depth).arg(r.out_w).arg(r.out_h)
                   .arg(in_ == out_ ? QStringLiteral("(原地)") : out_)
                   .arg(r.in_size).arg(r.out_size)
                   .arg(QString::number(r.secs, 'f', 2))
                   .arg(replaced ? QStringLiteral("，已删除原文件") : QString());
    } else {
        line = QString("[失败] %1: %2").arg(in_, QString::fromLocal8Bit(r.err));
    }

    QMetaObject::invokeMethod(win_, "logResult", Qt::QueuedConnection,
                              Q_ARG(QString, line));
}

/* ---------------- MainWindow ---------------- */

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), pending_(0), failures_(0)
{
    setWindowTitle("img2png - 图片转 PNG");
    setAcceptDrops(true);
    resize(720, 560);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QVBoxLayout(central);

    /* file list */
    fileList_ = new QListWidget;
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    root->addWidget(new QLabel("待转换文件/文件夹（可拖拽添加，支持 BMP/TGA/PNM/ICO/JPEG/PNG，"
                               "文件夹递归扫描）："));
    root->addWidget(fileList_, 2);

    auto *fileBtns = new QHBoxLayout;
    auto *addBtn = new QPushButton("添加文件…");
    auto *addDirBtn = new QPushButton("添加文件夹…");
    auto *clearBtn = new QPushButton("清空列表");
    fileBtns->addWidget(addBtn);
    fileBtns->addWidget(addDirBtn);
    fileBtns->addWidget(clearBtn);
    fileBtns->addStretch();
    root->addLayout(fileBtns);

    /* options */
    auto *opts = new QHBoxLayout;
    opts->addWidget(new QLabel("压缩级别(0-9):"));
    levelSlider_ = new QSlider(Qt::Horizontal);
    levelSlider_->setRange(0, 9);
    levelSlider_->setValue(9);
    levelSlider_->setMinimumWidth(160);
    QLabel *levelVal = new QLabel("9");
    connect(levelSlider_, &QSlider::valueChanged, levelVal,
            [levelVal](int v) { levelVal->setText(QString::number(v)); });
    opts->addWidget(levelSlider_);
    opts->addWidget(levelVal);

    opts->addWidget(new QLabel("滤镜:"));
    filterBox_ = new QComboBox;
    filterBox_->addItem("自适应", (int)PNGF_AUTO);
    filterBox_->addItem("无", (int)PNGF_NONE);
    filterBox_->addItem("Sub", (int)PNGF_SUB);
    filterBox_->addItem("Up", (int)PNGF_UP);
    filterBox_->addItem("Average", (int)PNGF_AVG);
    filterBox_->addItem("Paeth", (int)PNGF_PAETH);
    filterBox_->addItem("全部尝试", (int)PNGF_ALL);
    filterBox_->addItem("快速", (int)PNGF_FAST);
    opts->addWidget(filterBox_);

    opts->addWidget(new QLabel("线程数:"));
    threadsBox_ = new QSpinBox;
    threadsBox_->setRange(1, 256);
    threadsBox_->setValue(QThread::idealThreadCount());
    opts->addWidget(threadsBox_);
    root->addLayout(opts);

    auto *opts2 = new QHBoxLayout;
    strictDepthBox_ = new QCheckBox("严格匹配源图位深（不勾选则自动优化：去无用alpha/灰度检测）");
    strictDepthBox_->setChecked(true);
    keepTimeBox_ = new QCheckBox("输出文件时间戳与输入一致");
    keepTimeBox_->setChecked(true);
    overwriteBox_ = new QCheckBox("覆盖原文件（输出与输入相同时原地覆盖；同目录的非PNG转换成功后删除原文件）");
    opts2->addWidget(strictDepthBox_);
    opts2->addWidget(keepTimeBox_);
    opts2->addWidget(overwriteBox_);
    opts2->addStretch();
    root->addLayout(opts2);

    /* output dir */
    auto *outRow = new QHBoxLayout;
    outRow->addWidget(new QLabel("输出目录（留空 = PNG原地/同名生成）:"));
    outDirEdit_ = new DropPathEdit;
    outRow->addWidget(outDirEdit_, 1);
    auto *browseBtn = new QPushButton("浏览…");
    outRow->addWidget(browseBtn);
    root->addLayout(outRow);

    /* convert button + log */
    convertBtn_ = new QPushButton("开始转换");
    QFont bf = convertBtn_->font();
    bf.setBold(true);
    convertBtn_->setFont(bf);
    root->addWidget(convertBtn_);

    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    root->addWidget(logView_, 3);

    connect(addBtn, &QPushButton::clicked, this, &MainWindow::addFiles);
    connect(addDirBtn, &QPushButton::clicked, this, &MainWindow::addFolder);
    connect(clearBtn, &QPushButton::clicked, fileList_, &QListWidget::clear);
    connect(browseBtn, &QPushButton::clicked, this, &MainWindow::pickOutDir);
    connect(convertBtn_, &QPushButton::clicked, this, &MainWindow::runConversion);
}

void MainWindow::log(const QString &line)
{
    logView_->appendPlainText(line);
}

void MainWindow::logResult(const QString &line)
{
    log(line);
    if (line.startsWith("[失败]"))
        failures_++;
    if (--pending_ == 0) {
        convertBtn_->setEnabled(true);
        log(failures_ ? QString("完成：%1 个失败").arg(failures_)
                      : QString("完成：全部成功"));
        failures_ = 0;
    }
}

/* Expand a path (file or folder, recursive) into supported image files. */
static void collectImages(const QString &path, QStringList *out)
{
    QFileInfo fi(path);
    if (fi.isDir()) {
        QDirIterator it(path, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext())
            collectImages(it.next(), out);
        return;
    }
    if (!fi.isFile())
        return;
    QString ext = fi.suffix().toLower();
    static const char *kExts[] = { "bmp", "dib", "tga", "pnm", "ppm", "pgm",
                                   "pbm", "ico", "jpg", "jpeg", "jfif", "png" };
    for (const char *e : kExts) {
        if (ext == QLatin1String(e)) {
            out->append(QDir::toNativeSeparators(fi.absoluteFilePath()));
            return;
        }
    }
}

void MainWindow::addPath(const QString &path)
{
    QStringList files;
    collectImages(path, &files);
    int added = 0;
    for (const QString &f : files) {
        /* skip entries already in the list */
        bool dup = false;
        for (int i = 0; i < fileList_->count(); i++) {
            if (fileList_->item(i)->text() == f) { dup = true; break; }
        }
        if (!dup) {
            fileList_->addItem(f);
            added++;
        }
    }
    if (QFileInfo(path).isDir())
        log(QString("已从文件夹 %1 添加 %2 个图片文件").arg(path).arg(added));
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
        event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QList<QUrl> urls = event->mimeData()->urls();
    for (const QUrl &url : urls)
        addPath(url.toLocalFile());
    event->acceptProposedAction();
}

void MainWindow::addFiles()
{
    QStringList files = QFileDialog::getOpenFileNames(
        this, "选择要转换的图片", QString(),
        "图片 (*.bmp *.dib *.tga *.pnm *.ppm *.pgm *.pbm *.ico *.jpg *.jpeg *.jfif *.png);;所有文件 (*)");
    for (const QString &f : files)
        addPath(f);
}

void MainWindow::addFolder()
{
    QString dir = QFileDialog::getExistingDirectory(this, "选择要转换的文件夹");
    if (!dir.isEmpty())
        addPath(dir);
}

void MainWindow::pickOutDir()
{
    QString dir = QFileDialog::getExistingDirectory(this, "选择输出目录");
    if (!dir.isEmpty())
        outDirEdit_->setText(QDir::toNativeSeparators(dir));
}

void MainWindow::runConversion()
{
    if (pending_ > 0)
        return;

    int n = fileList_->count();
    if (n == 0) {
        log("没有待转换的文件。");
        return;
    }

    /* resolve output dir; empty = generate next to each input */
    QString outDir = outDirEdit_->text().trimmed();
    if (!outDir.isEmpty()) {
        QDir().mkpath(outDir);
        if (!outDir.endsWith('/') && !outDir.endsWith('\\'))
            outDir += '\\';
    }

    png_opts_t o;
    o.level = levelSlider_->value();
    o.filter = (png_filter_mode_t)filterBox_->currentData().toInt();
    o.auto_optimize = strictDepthBox_->isChecked() ? 0 : 1;

    QThreadPool *pool = QThreadPool::globalInstance();
    pool->setMaxThreadCount(threadsBox_->value());

    pending_ = n;
    failures_ = 0;
    convertBtn_->setEnabled(false);
    log(QString("开始转换 %1 个文件（级别 %2，滤镜 %3，线程 %4）…")
            .arg(n).arg(o.level).arg(filterBox_->currentText()).arg(threadsBox_->value()));

    for (int i = 0; i < n; i++) {
        QString in = fileList_->item(i)->text();
        QFileInfo fi(in);
        QString out;
        if (!outDir.isEmpty())
            out = outDir + fi.completeBaseName() + ".png";
        else
            out = fi.absolutePath() + "\\" + fi.completeBaseName() + ".png";

        /* refuse to clobber the source file unless explicitly allowed */
        if (!overwriteBox_->isChecked() &&
            QFileInfo(out).absoluteFilePath().compare(
                QFileInfo(in).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
            logResult(QString("[跳过] %1: 输出会覆盖原文件（勾选\"覆盖原文件\"可原地重压缩）")
                          .arg(in));
            continue;
        }

        auto *task = new ConvertTask(this, in, out, o.level, o.filter,
                                     o.auto_optimize != 0, keepTimeBox_->isChecked(),
                                     overwriteBox_->isChecked());
        pool->start(task);
    }
}
