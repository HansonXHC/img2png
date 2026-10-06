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
#include <QSettings>
#include <QThreadPool>
#include <QVBoxLayout>

/* ---------------- language plumbing ---------------- */

QString g_lang = QStringLiteral("zh");   // "zh" | "en", read by worker threads

QString tr2(const char *zh, const char *en)
{
    return g_lang == QLatin1String("en") ? QString::fromUtf8(en)
                                         : QString::fromUtf8(zh);
}

/* ---------------- DropPathEdit ---------------- */

DropPathEdit::DropPathEdit(QWidget *parent)
    : QLineEdit(parent)
{
    setAcceptDrops(true);
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
        /* "Overwrite originals" on: a non-PNG source whose .png landed in
         * the same folder is replaced by it (original deleted on success) */
        bool replaced = false;
        if (replace_ && !in_.endsWith(".png", Qt::CaseInsensitive) &&
            QFileInfo(out_).absolutePath() == QFileInfo(in_).absolutePath()) {
            replaced = QFile::remove(in_);
        }
        QString frameinfo;
        if (r.out_frames > 1)
            frameinfo = tr2("，%1 帧", ", %1 frames").arg(r.out_frames);
        line = tr2("[完成] %1 (%2 %3位 %4x%5) -> %6  %7 -> %8 字节, %9s%10%11",
                   "[done] %1 (%2 %3-bit %4x%5) -> %6  %7 -> %8 bytes, %9s%10%11")
                   .arg(in_, img2png_color_name(r.out_color))
                   .arg(r.out_depth).arg(r.out_w).arg(r.out_h)
                   .arg(in_ == out_ ? tr2("（原地）", " (in place)") : out_)
                   .arg(r.in_size).arg(r.out_size)
                   .arg(QString::number(r.secs, 'f', 2))
                   .arg(replaced ? tr2("，已删除原文件", ", original deleted") : QString())
                   .arg(frameinfo);
        if (!replaced && in_ == out_ && r.out_size >= r.in_size)
            line += tr2("（原文件已是最佳压缩）", " (source was already optimally compressed)");
    } else {
        line = tr2("[失败] %1: %2", "[fail] %1: %2")
                   .arg(in_, QString::fromLocal8Bit(r.err));
    }

    QMetaObject::invokeMethod(win_, "logResult", Qt::QueuedConnection,
                              Q_ARG(QString, line));
}

/* ---------------- MainWindow ---------------- */

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), pending_(0), failures_(0)
{
    QSettings settings;
    g_lang = settings.value("language", "zh").toString();
    if (g_lang != QLatin1String("en") && g_lang != QLatin1String("zh"))
        g_lang = QStringLiteral("zh");

    setAcceptDrops(true);
    resize(720, 560);

    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *root = new QVBoxLayout(central);

    /* file list */
    labelFileList_ = new QLabel;
    fileList_ = new QListWidget;
    fileList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    root->addWidget(labelFileList_);
    root->addWidget(fileList_, 2);

    auto *fileBtns = new QHBoxLayout;
    btnAdd_ = new QPushButton;
    btnAddDir_ = new QPushButton;
    btnClear_ = new QPushButton;
    fileBtns->addWidget(btnAdd_);
    fileBtns->addWidget(btnAddDir_);
    fileBtns->addWidget(btnClear_);
    fileBtns->addStretch();

    /* language selector (persisted) */
    fileBtns->addWidget(new QLabel(tr2("语言：", "Language:")));
    auto *langBox = new QComboBox;
    langBox->addItem(QStringLiteral("中文"), QStringLiteral("zh"));
    langBox->addItem(QStringLiteral("English"), QStringLiteral("en"));
    langBox->setCurrentIndex(g_lang == QLatin1String("en") ? 1 : 0);
    fileBtns->addWidget(langBox);
    root->addLayout(fileBtns);

    /* options */
    auto *opts = new QHBoxLayout;
    labelLevel_ = new QLabel;
    opts->addWidget(labelLevel_);
    levelSlider_ = new QSlider(Qt::Horizontal);
    levelSlider_->setRange(0, 9);
    levelSlider_->setValue(9);
    levelSlider_->setMinimumWidth(160);
    QLabel *levelVal = new QLabel("9");
    connect(levelSlider_, &QSlider::valueChanged, levelVal,
            [levelVal](int v) { levelVal->setText(QString::number(v)); });
    opts->addWidget(levelSlider_);
    opts->addWidget(levelVal);

    labelFilter_ = new QLabel;
    opts->addWidget(labelFilter_);
    filterBox_ = new QComboBox;
    opts->addWidget(filterBox_);

    labelThreads_ = new QLabel;
    opts->addWidget(labelThreads_);
    threadsBox_ = new QSpinBox;
    threadsBox_->setRange(1, 256);
    threadsBox_->setValue(QThread::idealThreadCount());
    opts->addWidget(threadsBox_);
    root->addLayout(opts);

    auto *opts2 = new QHBoxLayout;
    strictDepthBox_ = new QCheckBox;
    strictDepthBox_->setChecked(true);
    keepTimeBox_ = new QCheckBox;
    keepTimeBox_->setChecked(true);
    overwriteBox_ = new QCheckBox;
    opts2->addWidget(strictDepthBox_);
    opts2->addWidget(keepTimeBox_);
    opts2->addWidget(overwriteBox_);
    opts2->addStretch();
    root->addLayout(opts2);

    /* output dir */
    auto *outRow = new QHBoxLayout;
    labelOutDir_ = new QLabel;
    outRow->addWidget(labelOutDir_);
    outDirEdit_ = new DropPathEdit;
    outRow->addWidget(outDirEdit_, 1);
    btnBrowse_ = new QPushButton;
    outRow->addWidget(btnBrowse_);
    root->addLayout(outRow);

    /* convert button + log */
    convertBtn_ = new QPushButton;
    QFont bf = convertBtn_->font();
    bf.setBold(true);
    convertBtn_->setFont(bf);
    root->addWidget(convertBtn_);

    logView_ = new QPlainTextEdit;
    logView_->setReadOnly(true);
    root->addWidget(logView_, 3);

    connect(btnAdd_, &QPushButton::clicked, this, &MainWindow::addFiles);
    connect(btnAddDir_, &QPushButton::clicked, this, &MainWindow::addFolder);
    connect(btnClear_, &QPushButton::clicked, fileList_, &QListWidget::clear);
    connect(btnBrowse_, &QPushButton::clicked, this, &MainWindow::pickOutDir);
    connect(convertBtn_, &QPushButton::clicked, this, &MainWindow::runConversion);
    connect(langBox, &QComboBox::currentIndexChanged, this, [this, langBox](int index) {
        g_lang = langBox->itemData(index).toString();
        QSettings settings;
        settings.setValue("language", g_lang);
        applyTexts();
    });

    applyTexts();
}

void MainWindow::applyTexts()
{
    setWindowTitle(tr2("img2png - 图片转 PNG", "img2png - Image to PNG"));
    labelFileList_->setText(tr2("待转换文件/文件夹（可拖拽添加，支持 BMP/TGA/PNM/ICO/JPEG/PNG/GIF/QOI/WebP/TIFF，"
                                "文件夹递归扫描）：",
                                "Files/folders to convert (drag & drop; BMP/TGA/PNM/ICO/JPEG/PNG/GIF/QOI/WebP/TIFF; "
                                "folders are scanned recursively):"));
    btnAdd_->setText(tr2("添加文件…", "Add files…"));
    btnAddDir_->setText(tr2("添加文件夹…", "Add folder…"));
    btnClear_->setText(tr2("清空列表", "Clear list"));
    labelLevel_->setText(tr2("压缩级别(0-9):", "Compression level (0-9):"));
    labelFilter_->setText(tr2("滤镜:", "Filter:"));
    labelThreads_->setText(tr2("线程数:", "Threads:"));
    strictDepthBox_->setText(tr2("严格匹配源图位深（不勾选则自动优化：去无用alpha/灰度检测）",
                                 "Match source bit depth strictly (uncheck to auto-optimize: drop useless alpha / gray detection)"));
    keepTimeBox_->setText(tr2("输出文件时间戳与输入一致", "Keep input timestamps on output"));
    overwriteBox_->setText(tr2("覆盖原文件（输出与输入相同时原地覆盖；同目录的非PNG转换成功后删除原文件）",
                               "Overwrite originals (in-place when output==input; same-folder non-PNG sources are deleted after conversion)"));
    labelOutDir_->setText(tr2("输出目录（留空 = PNG原地/同名生成）:", "Output directory (empty = in-place / next to input):"));
    outDirEdit_->setPlaceholderText(tr2("可拖拽文件夹到此框作为输出目录",
                                        "Drop a folder here to use it as the output directory"));
    btnBrowse_->setText(tr2("浏览…", "Browse…"));
    convertBtn_->setText(tr2("开始转换", "Convert"));

    /* filter combo: rebuild items, keep selection */
    int cur = filterBox_->currentIndex();
    filterBox_->blockSignals(true);
    filterBox_->clear();
    filterBox_->addItem(tr2("自适应", "Adaptive"), (int)PNGF_AUTO);
    filterBox_->addItem(tr2("无", "None"), (int)PNGF_NONE);
    filterBox_->addItem("Sub", (int)PNGF_SUB);
    filterBox_->addItem("Up", (int)PNGF_UP);
    filterBox_->addItem("Average", (int)PNGF_AVG);
    filterBox_->addItem("Paeth", (int)PNGF_PAETH);
    filterBox_->addItem(tr2("全部尝试", "Try all"), (int)PNGF_ALL);
    filterBox_->addItem(tr2("快速", "Fast"), (int)PNGF_FAST);
    filterBox_->setCurrentIndex(cur < 0 ? 0 : cur);
    filterBox_->blockSignals(false);
}

void MainWindow::onLanguageChanged()
{
    applyTexts();
}

void MainWindow::log(const QString &line)
{
    logView_->appendPlainText(line);
}

void MainWindow::logResult(const QString &line)
{
    log(line);
    if (line.startsWith("[失败]") || line.startsWith("[fail]"))
        failures_++;
    if (--pending_ == 0) {
        convertBtn_->setEnabled(true);
        log(failures_ ? tr2("完成：%1 个失败", "Done: %1 failed").arg(failures_)
                      : tr2("完成：全部成功", "Done: all succeeded"));
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
                                   "pbm", "ico", "jpg", "jpeg", "jfif", "png",
                                   "gif", "qoi", "webp", "tif", "tiff" };
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
        log(tr2("已从文件夹 %1 添加 %2 个图片文件", "Added %2 image file(s) from folder %1")
                .arg(added).arg(path));
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
        this, tr2("选择要转换的图片", "Select images to convert"), QString(),
        tr2("图片 (*.bmp *.dib *.tga *.pnm *.ppm *.pgm *.pbm *.ico *.jpg *.jpeg *.jfif *.png *.gif *.qoi *.webp *.tif *.tiff);;所有文件 (*)",
            "Images (*.bmp *.dib *.tga *.pnm *.ppm *.pgm *.pbm *.ico *.jpg *.jpeg *.jfif *.png *.gif *.qoi *.webp *.tif *.tiff);;All files (*)"));
    for (const QString &f : files)
        addPath(f);
}

void MainWindow::addFolder()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr2("选择要转换的文件夹", "Select folder to convert"));
    if (!dir.isEmpty())
        addPath(dir);
}

void MainWindow::pickOutDir()
{
    QString dir = QFileDialog::getExistingDirectory(this, tr2("选择输出目录", "Select output directory"));
    if (!dir.isEmpty())
        outDirEdit_->setText(QDir::toNativeSeparators(dir));
}

void MainWindow::runConversion()
{
    if (pending_ > 0)
        return;

    int n = fileList_->count();
    if (n == 0) {
        log(tr2("没有待转换的文件。", "Nothing to convert."));
        return;
    }

    /* resolve output dir; empty = generate next to each input */
    QString outDir = outDirEdit_->text().trimmed();
    if (!outDir.isEmpty()) {
        QDir().mkpath(outDir);
        if (!outDir.endsWith('/') && !outDir.endsWith('\\'))
            outDir += '/';
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
    log(tr2("开始转换 %1 个文件（级别 %2，滤镜 %3，线程 %4）…",
            "Converting %1 files (level %2, filter %3, threads %4)…")
            .arg(n).arg(o.level).arg(filterBox_->currentText()).arg(threadsBox_->value()));

    for (int i = 0; i < n; i++) {
        QString in = fileList_->item(i)->text();
        QFileInfo fi(in);
        QString out;
        if (!outDir.isEmpty())
            out = outDir + fi.completeBaseName() + ".png";
        else
            out = fi.absolutePath() + '/' + fi.completeBaseName() + ".png";

        /* refuse to clobber the source file unless explicitly allowed */
        if (!overwriteBox_->isChecked() &&
            QFileInfo(out).absoluteFilePath().compare(
                QFileInfo(in).absoluteFilePath(), Qt::CaseInsensitive) == 0) {
            logResult(tr2("[跳过] %1: 输出会覆盖原文件（勾选\"覆盖原文件\"可原地重压缩）",
                          "[skip] %1: output would overwrite the source (enable \"Overwrite originals\" to recompress in place)")
                          .arg(in));
            continue;
        }

        auto *task = new ConvertTask(this, in, out, o.level, o.filter,
                                     o.auto_optimize != 0, keepTimeBox_->isChecked(),
                                     overwriteBox_->isChecked());
        pool->start(task);
    }
}
