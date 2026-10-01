#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QLineEdit>
#include <QRunnable>
#include <QCheckBox>
#include <QComboBox>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>

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

private:
    void log(const QString &line);
    void addPath(const QString &path);

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
