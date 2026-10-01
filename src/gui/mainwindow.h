#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLabel>
#include <QLineEdit>
#include <QRunnable>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>

/* Current UI language: "zh" or "en" (shared with worker threads). */
extern QString g_lang;

/* Pick a string by current language: tr2("中文", "English"). */
QString tr2(const char *zh, const char *en);

/* Line edit that accepts a dragged folder (or file -> its parent dir)
 * and fills itself with that path. */
class DropPathEdit : public QLineEdit
{
    Q_OBJECT

public:
    explicit DropPathEdit(QWidget *parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void addFiles();
    void addFolder();
    void pickOutDir();
    void runConversion();
    void logResult(const QString &line);
    void onLanguageChanged();

private:
    void log(const QString &line);
    void addPath(const QString &path);
    void applyTexts();

    QListWidget *fileList_;
    QSlider *levelSlider_;
    QComboBox *filterBox_;
    QSpinBox *threadsBox_;
    QCheckBox *strictDepthBox_;
    QCheckBox *keepTimeBox_;
    QCheckBox *overwriteBox_;
    DropPathEdit *outDirEdit_;
    QPushButton *convertBtn_;
    QPlainTextEdit *logView_;

    /* translatable widgets */
    QLabel *labelFileList_;
    QPushButton *btnAdd_;
    QPushButton *btnAddDir_;
    QPushButton *btnClear_;
    QLabel *labelLevel_;
    QLabel *labelFilter_;
    QLabel *labelThreads_;
    QLabel *labelOutDir_;
    QPushButton *btnBrowse_;

    int pending_;
    int failures_;
};

/* one conversion task for QThreadPool */
class ConvertTask : public QRunnable
{
public:
    ConvertTask(MainWindow *win, QString in, QString out,
                int level, int filter, bool autoOpt, bool keepTime,
                bool replaceOriginal);
    void run() override;

private:
    MainWindow *win_;
    QString in_, out_;
    int level_, filter_;
    bool autoOpt_, keepTime_, replace_;
};

#endif
