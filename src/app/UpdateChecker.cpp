#include "UpdateChecker.h"

#include "Edition.h"
#include "Lang.h"
#include "platform/Platform.h"

#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QThread>
#include <QVBoxLayout>

namespace {

QString apiUrl() { return QStringLiteral("https://api.github.com/repos/%1/releases/latest").arg(Edition::repository()); }

QList<int> versionParts(QString v, QString* suffix)
{
    v = v.trimmed();
    if (v.startsWith(QLatin1Char('v'), Qt::CaseInsensitive)) v.remove(0, 1);
    const int dash = v.indexOf(QLatin1Char('-'));
    if (suffix) *suffix = dash >= 0 ? v.mid(dash + 1) : QString();
    if (dash >= 0) v.truncate(dash);
    QList<int> parts;
    for (const QString& p : v.split(QLatin1Char('.'))) parts << p.toInt();
    while (parts.size() < 3) parts << 0;
    return parts;
}

QString formatSize(qint64 bytes) { return QStringLiteral("%1 MB").arg(bytes / double(1 << 20), 0, 'f', 1); }

} // namespace

UpdateChecker::UpdateChecker(QWidget* window)
    : QObject(window)
    , m_window(window)
{
}

QString UpdateChecker::currentVersion() { return QStringLiteral(CHARONOS_SEMVER); }

int UpdateChecker::compareVersions(const QString& a, const QString& b)
{
    QString sa, sb;
    const QList<int> pa = versionParts(a, &sa), pb = versionParts(b, &sb);
    for (int i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const int x = pa.value(i), y = pb.value(i);
        if (x != y) return x < y ? -1 : 1;
    }
    // 1.2.0-beta < 1.2.0
    if (sa.isEmpty() != sb.isEmpty()) return sa.isEmpty() ? 1 : -1;
    return QString::compare(sa, sb, Qt::CaseInsensitive) < 0 ? -1 : (sa == sb ? 0 : 1);
}

int UpdateChecker::pickAsset(const QStringList& names, bool lite, bool android)
{
    for (int i = 0; i < names.size(); ++i) {
        const QString n = names[i].toLower();
        const bool isLite = n.contains(QLatin1String("lite"));
        if (android) {
            if (n.endsWith(QLatin1String(".apk"))) return i;
            continue;
        }
        if (isLite == lite && n.endsWith(QLatin1String(".exe")) && n.contains(QLatin1String("setup"))) return i;
    }
    if (android) return -1;
    for (int i = 0; i < names.size(); ++i) {  // no installer: the portable ZIP of this edition
        const QString n = names[i].toLower();
        if (n.contains(QLatin1String("lite")) == lite && n.contains(QLatin1String("win64")) && n.endsWith(QLatin1String(".zip"))) return i;
    }
    return -1;
}

UpdateChecker::Release UpdateChecker::parseRelease(const QByteArray& json, bool lite, bool android, QString* error)
{
    Release r;
    QJsonParseError pe{};
    const QJsonObject o = QJsonDocument::fromJson(json, &pe).object();
    if (pe.error != QJsonParseError::NoError || !o.contains(QLatin1String("tag_name"))) {
        if (error) *error = o.value(QLatin1String("message")).toString(QStringLiteral("Unexpected reply from GitHub"));
        return r;
    }
    r.tag = o.value(QLatin1String("tag_name")).toString();
    r.version = r.tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive) ? r.tag.mid(1) : r.tag;
    r.name = o.value(QLatin1String("name")).toString(r.tag);
    r.notes = o.value(QLatin1String("body")).toString();
    r.page = o.value(QLatin1String("html_url")).toString(Edition::releasesPage());
    const QJsonArray assets = o.value(QLatin1String("assets")).toArray();
    QStringList names;
    for (const QJsonValue& a : assets) names << a.toObject().value(QLatin1String("name")).toString();
    if (const int i = pickAsset(names, lite, android); i >= 0) {
        const QJsonObject a = assets[i].toObject();
        r.assetName = names[i];
        r.assetUrl = a.value(QLatin1String("browser_download_url")).toString();
        r.assetSize = qint64(a.value(QLatin1String("size")).toDouble());
    }
    return r;
}

void UpdateChecker::check(bool manual)
{
    if (m_busy) return;
    m_busy = true;
    if (Platform::isAndroid()) {
        // No TLS in Qt Network here: the request goes through the Android HTTP stack.
        QPointer<UpdateChecker> self(this);
        QThread* t = QThread::create([self, manual] {
            QString error;
            const QByteArray body = Platform::httpGet(apiUrl(), 15000, &error);
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, body, error, manual] {
                if (self) self->handle(body, error, manual);
            }, Qt::QueuedConnection);
        });
        connect(t, &QThread::finished, t, &QObject::deleteLater);
        t->start();
        return;
    }
    if (!m_net) m_net = new QNetworkAccessManager(this);
    QNetworkRequest req{ QUrl(apiUrl()) };
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", QByteArray("CHARONOS/") + currentVersion().toUtf8());
    req.setTransferTimeout(15000);
    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, manual] {
        reply->deleteLater();
        const QByteArray body = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError || !body.isEmpty() ? QString() : reply->errorString();
        handle(body, error, manual);
    });
}

void UpdateChecker::handle(const QByteArray& json, const QString& networkError, bool manual)
{
    m_busy = false;
    QString error = networkError;
    Release r;
    if (error.isEmpty()) r = parseRelease(json, Edition::lite, Platform::isAndroid(), &error);
    QSettings s;
    if (!error.isEmpty()) {
        qInfo().noquote() << "Update check failed:" << error;
        if (manual)
            QMessageBox::warning(m_window, L("Check for Updates", "업데이트 확인"),
                                 L("Couldn't reach GitHub.\n%1", "GitHub에 연결할 수 없습니다.\n%1").arg(error));
        return;
    }
    s.setValue(QStringLiteral("update/lastCheck"), QDateTime::currentDateTimeUtc());
    if (compareVersions(r.version, currentVersion()) <= 0) {
        if (manual)
            QMessageBox::information(m_window, L("Check for Updates", "업데이트 확인"),
                                     L("%1 %2 is the latest version.", "%1 %2은(는) 최신 버전입니다.").arg(Edition::productName(), currentVersion()));
        return;
    }
    // "Later" on the startup prompt silences that version until a newer one appears.
    if (!manual && s.value(QStringLiteral("update/skippedVersion")).toString() == r.tag) return;
    prompt(r, manual);
}

void UpdateChecker::prompt(const Release& r, bool manual)
{
    QDialog dlg(m_window);
    dlg.setObjectName(QStringLiteral("UpdateDialog"));
    dlg.setWindowTitle(L("Update Available", "업데이트 알림"));
    dlg.setMinimumWidth(520);
    auto* v = new QVBoxLayout(&dlg);
    v->setContentsMargins(22, 20, 22, 18);
    v->setSpacing(12);
    auto* title = new QLabel(L("<b style='font-size:15px'>%1 %2 is available</b>", "<b style='font-size:15px'>%1 %2 버전이 나왔습니다</b>")
                                 .arg(Edition::productName(), r.version.toHtmlEscaped()));
    title->setTextFormat(Qt::RichText);
    auto* sub = new QLabel(L("You have %1.", "현재 버전: %1").arg(currentVersion()) +
                           (r.assetName.isEmpty() ? QString() : QStringLiteral("  ·  %1 (%2)").arg(r.assetName, formatSize(r.assetSize))));
    sub->setObjectName(QStringLiteral("Muted"));
    auto* notes = new QTextBrowser;
    notes->setOpenExternalLinks(true);
    notes->setMarkdown(r.notes.isEmpty() ? r.name : r.notes);
    notes->setMinimumHeight(220);
    v->addWidget(title);
    v->addWidget(sub);
    v->addWidget(notes, 1);

    auto* later = new QPushButton(manual ? L("Close", "닫기") : L("Later (skip this version)", "나중에 (이 버전 건너뛰기)"));
    auto* page = new QPushButton(L("Release Page", "릴리스 페이지"));
    auto* now = new QPushButton(Platform::isAndroid() ? L("Download Update", "업데이트 다운로드") : L("Update Now", "지금 업데이트"));
    now->setDefault(true);
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(page);
    buttons->addStretch(1);
    buttons->addWidget(later);
    buttons->addWidget(now);
    v->addLayout(buttons);
    connect(later, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(page, &QPushButton::clicked, &dlg, [&] { Platform::openExternal(r.page); });
    connect(now, &QPushButton::clicked, &dlg, &QDialog::accept);

    if (dlg.exec() == QDialog::Accepted) {
        QSettings().remove(QStringLiteral("update/skippedVersion"));
        install(r);
    } else if (!manual) {
        QSettings().setValue(QStringLiteral("update/skippedVersion"), r.tag);
    }
}

void UpdateChecker::install(const Release& r)
{
    if (r.assetUrl.isEmpty() || Platform::isAndroid() || !r.assetName.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive)) {
        // Android hands the APK to the browser / package installer; a ZIP is unpacked by hand.
        Platform::openExternal(r.assetUrl.isEmpty() ? r.page : r.assetUrl);
        return;
    }
    if (!m_net) m_net = new QNetworkAccessManager(this);
    const QString dir = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("charonos-update"));
    QDir().mkpath(dir);
    const QString target = QDir(dir).filePath(r.assetName);
    auto* file = new QFile(target, this);
    if (!file->open(QIODevice::WriteOnly)) {
        delete file;
        Platform::openExternal(r.assetUrl);
        return;
    }
    QNetworkRequest req{ QUrl(r.assetUrl) };
    req.setRawHeader("User-Agent", QByteArray("CHARONOS/") + currentVersion().toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = m_net->get(req);
    auto* progress = new QProgressDialog(L("Downloading %1...", "%1 다운로드 중...").arg(r.assetName), L("Cancel", "취소"), 0, 1000, m_window);
    progress->setWindowTitle(L("Update", "업데이트"));
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setValue(0);
    connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::readyRead, file, [file, reply] { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, progress, [progress, total = r.assetSize](qint64 got, qint64 size) {
        const qint64 all = size > 0 ? size : total;
        if (all > 0) progress->setValue(int(std::min<qint64>(1000, got * 1000 / all)));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, progress, target, r] {
        file->write(reply->readAll());
        file->close();
        const bool ok = reply->error() == QNetworkReply::NoError && file->size() > 0;
        file->deleteLater();
        reply->deleteLater();
        progress->deleteLater();
        if (!ok) {
            QFile::remove(target);
            if (reply->error() != QNetworkReply::OperationCanceledError)
                QMessageBox::warning(m_window, L("Update", "업데이트"), L("Download failed: %1", "다운로드 실패: %1").arg(reply->errorString()));
            return;
        }
        if (QMessageBox::question(m_window, L("Update", "업데이트"),
                                  L("The installer is ready. Close CHARONOS and install %1 now?", "설치 파일이 준비됐습니다. CHARONOS를 닫고 %1을(를) 설치할까요?").arg(r.version))
            != QMessageBox::Yes)
            return;
        if (!QProcess::startDetached(target, {})) {
            Platform::openExternal(QFileInfo(target).absolutePath());
            return;
        }
        m_window->close();  // asks to save first; the installer closes a CHARONOS that is still running
    });
}
