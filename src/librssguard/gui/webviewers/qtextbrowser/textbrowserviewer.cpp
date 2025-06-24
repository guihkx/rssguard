// For license of this file, see <project-root-folder>/LICENSE.md.

#include "gui/webviewers/qtextbrowser/textbrowserviewer.h"

#include "3rd-party/boolinq/boolinq.h"
#include "definitions/globals.h"
#include "gui/dialogs/formmain.h"
#include "gui/webbrowser.h"
#include "miscellaneous/application.h"
#include "miscellaneous/externaltool.h"
#include "miscellaneous/iconfactory.h"
#include "miscellaneous/settings.h"
#include "network-web/adblock/adblockrequestinfo.h"
#include "network-web/downloader.h"
#include "network-web/networkfactory.h"
#include "network-web/webfactory.h"

#include <QBuffer>
#include <QContextMenuEvent>
#include <QFileIconProvider>
#include <QScrollBar>
#include <QTextCodec>
#include <QTimer>
#include <QtConcurrent>

TextBrowserViewer::TextBrowserViewer(QWidget* parent)
  : QTextBrowser(parent), m_resourcesEnabled(false), m_resourceDownloader(new Downloader()),
    m_resourceDownloaderThread(new QThread(this)), m_loadedResources({}),
    m_placeholderImage(qApp->icons()->miscPixmap(QSL("image-placeholder"))),
    m_placeholderImageError(qApp->icons()->miscPixmap(QSL("image-placeholder-error"))),
    m_downloader(new Downloader(this)), m_document(new TextBrowserDocument(this)) {
  setAutoFillBackground(false);
  setFrameShape(QFrame::Shape::NoFrame);
  setFrameShadow(QFrame::Shadow::Plain);
  setTabChangesFocus(true);
  setOpenLinks(false);
  setWordWrapMode(QTextOption::WrapMode::WordWrap);

  viewport()->setAutoFillBackground(false);

  setResourcesEnabled(qApp->settings()->value(GROUP(Messages), SETTING(Messages::ShowResourcesInArticles)).toBool());
  setDocument(m_document.data());

  m_resourceDownloader->moveToThread(m_resourceDownloaderThread);
  m_resourceDownloaderThread->start(QThread::Priority::LowPriority);

  connect(this, &TextBrowserViewer::reloadDocument, this, [this]() {
    const auto scr = verticalScrollBarPosition();
    setHtmlPrivate(html(), m_currentUrl);
    setVerticalScrollBarPosition(scr);
  });

  connect(m_resourceDownloader, &Downloader::completed, this, &TextBrowserViewer::resourceDownloaded);
  connect(this, &QTextBrowser::anchorClicked, this, &TextBrowserViewer::onAnchorClicked);
  connect(this, QOverload<const QUrl&>::of(&QTextBrowser::highlighted), this, &TextBrowserViewer::linkMouseHighlighted);
}

TextBrowserViewer::~TextBrowserViewer() {
  m_resourceDownloaderThread->quit();
  m_resourceDownloaderThread->wait();
  m_resourceDownloaderThread->deleteLater();
  m_resourceDownloader->deleteLater();
}

QSize TextBrowserViewer::sizeHint() const {
  auto doc_size = document()->size().toSize();

  doc_size.setHeight(doc_size.height() + contentsMargins().top() + contentsMargins().bottom());
  return doc_size;
}

QVariant TextBrowserViewer::loadOneResource(int type, const QUrl& name) {
  if (type != QTextDocument::ResourceType::ImageResource) {
    return {};
  }

  auto resolved_name = (m_currentUrl.isValid() && name.isRelative()) ? m_currentUrl.resolved(name) : name;

  if (!m_resourcesEnabled || !m_loadedResources.contains(resolved_name)) {
    // Resources are not enabled.
    return m_placeholderImage;
  }

  // Resources are enabled and we already have the resource.
  int acceptable_width = int(width() * ACCEPTABLE_IMAGE_PERCENTUAL_WIDTH);

  QMap<int, QByteArray>& resource_data_all_sizes = m_loadedResources[resolved_name];
  QImage img;

  qDebugNN << LOGSEC_GUI << "Picture" << QUOTE_W_SPACE(name)
           << "has these sizes cached:" << NONQUOTE_W_SPACE_DOT(resource_data_all_sizes.keys());

  if (resource_data_all_sizes.isEmpty()) {
    img = m_placeholderImageError.toImage();
  }
  else {
    // Now, we either select specifically sized picture, or default one.
    QByteArray resource_data;

    if (resource_data_all_sizes.contains(acceptable_width)) {
      // We have picture with this exact size. The picture was likely downsized
      // to this size before.
      resource_data = resource_data_all_sizes.value(acceptable_width);
    }
    else {
      // We only have default size or not desired size. Return initial picture.
      resource_data = resource_data_all_sizes.value(0);
    }

    img = QImage::fromData(resource_data);
  }

  int img_width = img.width();

  if (img_width > acceptable_width) {
    QElapsedTimer tmr;

    tmr.start();
    img = img.scaledToWidth(acceptable_width, Qt::TransformationMode::SmoothTransformation);

    qWarningNN << LOGSEC_GUI << "Picture" << QUOTE_W_SPACE(name) << "with width" << QUOTE_W_SPACE(img_width)
               << "is too wide, down-scaling to prevent horizontal scrollbars. Scaling took"
               << NONQUOTE_W_SPACE(tmr.elapsed()) << "miliseconds.";

    QByteArray save_arr;
    QBuffer save_buf(&save_arr, this);

    if (img.save(&save_buf, "JPG", 100)) {
      save_buf.close();

      /*
      IOFactory::writeFile(QSL("%1%2.jpg")
                             .arg(name.toString(QUrl::ComponentFormattingOption::FullyEncoded),
                                  QString::number(acceptable_width))
                             .remove(QRegularExpression(":|\\/")),
                           save_arr);
*/

      resource_data_all_sizes.insert(acceptable_width, save_arr);
    }
    else {
      qWarningNN << LOGSEC_GUI << "Failed to save modified image" << QUOTE_W_SPACE(name) << "to cache.";
    }
  }

  return img;
}

void TextBrowserViewer::bindToBrowser(WebBrowser* browser) {
  installEventFilter(browser);

  browser->m_actionBack = nullptr;
  browser->m_actionForward = nullptr;
  browser->m_actionReload = nullptr;
  browser->m_actionStop = nullptr;
}

void TextBrowserViewer::findText(const QString& text, bool backwards) {
  if (!text.isEmpty()) {
    bool found =
      QTextBrowser::find(text, backwards ? QTextDocument::FindFlag::FindBackward : QTextDocument::FindFlag(0));

    if (!found) {
      textCursor().clearSelection();
      moveCursor(QTextCursor::MoveOperation::Start);

      QTextBrowser::find(text, backwards ? QTextDocument::FindFlag::FindBackward : QTextDocument::FindFlag(0));
    }
  }
  else {
    textCursor().clearSelection();
    moveCursor(QTextCursor::MoveOperation::Start);
  }
}

BlockingResult TextBrowserViewer::blockedWithAdblock(const QUrl& url) {
  AdblockRequestInfo block_request(url);

  if (url.path().endsWith(QSL("css"))) {
    block_request.setResourceType(QSL("stylesheet"));
  }
  else {
    block_request.setResourceType(QSL("image"));
  }

  auto block_result = qApp->web()->adBlock()->block(block_request);

  if (block_result.m_blocked) {
    qWarningNN << LOGSEC_ADBLOCK << "Blocked request:" << QUOTE_W_SPACE_DOT(block_request.requestUrl().toString());
    return block_result;
  }
  else {
    return block_result;
  }
}

void TextBrowserViewer::setUrl(const QUrl& url) {
  emit loadingStarted();

  QString html_str;
  QUrl nonconst_url = url;
  bool is_error = false;
  auto block_result = blockedWithAdblock(url);

  if (block_result.m_blocked) {
    is_error = true;
    nonconst_url = QUrl::fromUserInput(QSL(INTERNAL_URL_ADBLOCKED));

    html_str = QSL("Blocked!!!<br/>%1").arg(url.toString());
  }
  else {
    QEventLoop loop;

    connect(m_downloader.data(),
            &Downloader::completed,
            &loop,
            &QEventLoop::quit,
            Qt::ConnectionType(Qt::ConnectionType::UniqueConnection | Qt::ConnectionType::AutoConnection));
    m_downloader->manipulateData(url.toString(), QNetworkAccessManager::Operation::GetOperation, {}, 5000);

    loop.exec();

    const auto net_error = m_downloader->lastOutputError();
    const QString content_type = m_downloader->lastContentType();

    if (net_error != QNetworkReply::NetworkError::NoError) {
      is_error = true;
      html_str = QSL("Error!<br/>%1").arg(NetworkFactory::networkErrorText(net_error));
    }
    else {
      if (content_type.startsWith(QSL("image/"))) {
        html_str = QSL("<img src=\"%1\">").arg(nonconst_url.toString());
      }
      else {
        html_str = decodeHtmlData(m_downloader->lastOutputData(), content_type);
      }
    }
  }

  setHtml(html_str, nonconst_url);

  emit loadingFinished(!is_error);
}

QString TextBrowserViewer::decodeHtmlData(const QByteArray& data, const QString& content_type) const {
  QString found_charset = QRegularExpression("charset=([0-9a-zA-Z-_]+)").match(content_type).captured(1);
  QTextCodec* codec = QTextCodec::codecForName(found_charset.toLocal8Bit());

  if (codec == nullptr) {
    // No suitable codec for this encoding was found.
    // Use UTF-8.
    qWarningNN << LOGSEC_GUI << "Did not find charset for content-type" << QUOTE_W_SPACE_DOT(content_type);
    return QString::fromUtf8(data);
  }
  else {
    qDebugNN << LOGSEC_GUI << "Found charset for content-type" << QUOTE_W_SPACE_DOT(content_type);
    return codec->toUnicode(data);
  }
}

QString TextBrowserViewer::html() const {
  return m_currentHtml;
}

QUrl TextBrowserViewer::url() const {
  return m_currentUrl;
}

void TextBrowserViewer::clear() {
  setHtml({});
}

void TextBrowserViewer::loadMessages(const QList<Message>& messages, RootItem* root) {
  emit loadingStarted();
  m_root = root;

  auto html_messages = htmlForMessages(messages, root);

  /*
  // Replace base64 images.
  QRegularExpression exp_base64("src=\"data: ?image\\/[^;]+;base64,([^\"]+)\"");
  QRegularExpressionMatch exp_base64_match;

  while ((exp_base64_match = exp_base64.match(html_messages.m_html)).hasMatch()) {
    QString base64_img = exp_base64_match.captured(1);
    QByteArray data_img = QByteArray::fromBase64Encoding(base64_img);
  }
  */

#if !defined(NDEBUG)
  // IOFactory::writeFile("aaa.html", html_messages.m_html.toUtf8());
#endif

  setHtml(html_messages.m_html, html_messages.m_baseUrl);

  QTextOption op;
  op.setTextDirection((messages.at(0).m_rtlBehavior == RtlBehavior::Everywhere ||
                       messages.at(0).m_rtlBehavior == RtlBehavior::EverywhereExceptFeedList ||
                       messages.at(0).m_rtlBehavior == RtlBehavior::OnlyViewer)
                        ? Qt::LayoutDirection::RightToLeft
                        : Qt::LayoutDirection::LeftToRight);
  document()->setDefaultTextOption(op);

  emit loadingFinished(true);
}

PreparedHtml TextBrowserViewer::htmlForMessages(const QList<Message>& messages, RootItem* root) const {
  auto html_messages =
    qApp->settings()->value(GROUP(Messages), SETTING(Messages::UseLegacyArticleFormat)).toBool()
      ? prepareLegacyHtmlForMessage(messages, root)
      : qApp->skins()->generateHtmlOfArticles(messages, root, width() * ACCEPTABLE_IMAGE_PERCENTUAL_WIDTH);

  // Remove other characters which cannot be displayed properly.
  static QRegularExpression exp_symbols("&#x1F[0-9A-F]{3};");

  html_messages.m_html = html_messages.m_html.replace(exp_symbols, QString());

  return html_messages;
}

double TextBrowserViewer::verticalScrollBarPosition() const {
  return verticalScrollBar()->value();
}

void TextBrowserViewer::setVerticalScrollBarPosition(double pos) {
  verticalScrollBar()->setValue(int(pos));
}

void TextBrowserViewer::applyFont(const QFont& fon) {
  m_baseFont = fon;
  setFont(fon);
  setZoomFactor(zoomFactor());
}

qreal TextBrowserViewer::zoomFactor() const {
  return m_zoomFactor;
}

void TextBrowserViewer::setZoomFactor(qreal zoom_factor) {
  m_zoomFactor = zoom_factor;

  auto fon = font();

  fon.setPointSizeF(m_baseFont.pointSizeF() * zoom_factor);

  setFont(fon);
}

void TextBrowserViewer::contextMenuEvent(QContextMenuEvent* event) {
  event->accept();

  auto* menu = createStandardContextMenu(event->pos());

  if (menu == nullptr) {
    return;
  }

  /*
  connect(menu, &QMenu::aboutToHide, this, [menu] {
    menu->deleteLater();
  });*/

  if (m_actionEnableResources.isNull()) {
    m_actionEnableResources.reset(new QAction(qApp->icons()->fromTheme(QSL("viewimage"), QSL("image-x-generic")),
                                              tr("Enable external resources"),
                                              this));
    m_actionDownloadLink.reset(new QAction(qApp->icons()->fromTheme(QSL("download")), tr("Download"), this));

    m_actionEnableResources.data()->setCheckable(true);
    m_actionEnableResources.data()->setChecked(resourcesEnabled());

    connect(m_actionDownloadLink.data(), &QAction::triggered, this, &TextBrowserViewer::downloadLink);
    connect(m_actionEnableResources.data(), &QAction::toggled, this, &TextBrowserViewer::enableResources);
  }

  menu->addAction(m_actionEnableResources.data());
  menu->addAction(m_actionDownloadLink.data());

  auto anchor = anchorAt(event->pos());

  m_lastContextMenuPos = event->pos();
  m_actionDownloadLink.data()->setEnabled(!anchor.isEmpty());

  processContextMenu(menu, event);

  menu->popup(event->globalPos());
}

void TextBrowserViewer::resizeEvent(QResizeEvent* event) {
  // Notify parents about changed geometry.
  updateGeometry();
  QTextBrowser::resizeEvent(event);
}

void TextBrowserViewer::wheelEvent(QWheelEvent* event) {
  // NOTE: Skip base class implemetation.
  QAbstractScrollArea::wheelEvent(event);
  updateMicroFocus();
}

void TextBrowserViewer::enableResources(bool enable) {
  qApp->settings()->setValue(GROUP(Messages), Messages::ShowResourcesInArticles, enable);
  setResourcesEnabled(enable);
}

void TextBrowserViewer::downloadLink() {
  auto url = QUrl(anchorAt(m_lastContextMenuPos));

  if (url.isValid()) {
    const QUrl resolved_url = (m_currentUrl.isValid() && url.isRelative()) ? m_currentUrl.resolved(url) : url;

    qApp->downloadManager()->download(resolved_url);
  }
}

void TextBrowserViewer::onAnchorClicked(const QUrl& url) {
  if (!url.isEmpty()) {
    const QUrl resolved_url = (m_currentUrl.isValid() && url.isRelative()) ? m_currentUrl.resolved(url) : url;
    const bool ctrl_pressed =
      Globals::hasFlag(QGuiApplication::keyboardModifiers(), Qt::KeyboardModifier::ControlModifier);

    if (ctrl_pressed) {
      // Open in new tab.
      qApp->mainForm()->tabWidget()->addLinkedBrowser(resolved_url);
    }
    else {
      bool open_externally_now =
        qApp->settings()->value(GROUP(Browser), SETTING(Browser::OpenLinksInExternalBrowserRightAway)).toBool();

      if (open_externally_now) {
        qApp->web()->openUrlInExternalBrowser(resolved_url.toString());

        if (qApp->settings()
              ->value(GROUP(Messages), SETTING(Messages::BringAppToFrontAfterMessageOpenedExternally))
              .toBool()) {
          QTimer::singleShot(1000, qApp, []() {
            qApp->mainForm()->display();
          });
        }
      }
      else {
        setUrl(resolved_url);
      }
    }
  }
}

void TextBrowserViewer::setHtml(const QString& html, const QUrl& base_url) {
  if (m_resourcesEnabled) {
    // NOTE: This regex is problematic as it does not work for ALL
    // HTMLs, maybe use XML parsing to extract what we need?
    static QRegularExpression img_tag_rgx("\\<img[^\\>]*src\\s*=\\s*[\"\']([^\"\']*)[\"\'][^\\>]*\\>",
                                          QRegularExpression::PatternOption::CaseInsensitiveOption);
    QRegularExpressionMatchIterator i = img_tag_rgx.globalMatch(html);
    QList<QUrl> found_resources;

    while (i.hasNext()) {
      QRegularExpressionMatch match = i.next();
      auto captured_url = QUrl(match.captured(1));
      auto resolved_captured_url =
        (base_url.isValid() && captured_url.isRelative()) ? base_url.resolved(captured_url) : captured_url;

      if (!found_resources.contains(resolved_captured_url)) {
        found_resources.append(resolved_captured_url);
      }
    }

    auto really_needed_resources = boolinq::from(found_resources)
                                     .where([this](const QUrl& res) {
                                       return !m_loadedResources.contains(res);
                                     })
                                     .toStdList();

    m_neededResources = FROM_STD_LIST(QList<QUrl>, really_needed_resources);
  }
  else {
    m_neededResources = {};
  }

  setHtmlPrivate(html, base_url);

  /*
  QTextCursor cr(m_document.data());

  cr.movePosition(QTextCursor::MoveOperation::Start);

  // this can be used instead of regexps, just browse document and collect resource addresses directly
  while (!cr.atEnd()) {
    if (!cr.movePosition(QTextCursor::MoveOperation::NextBlock)) {
      break;
    }

    QTextBlock::iterator it;
    for (it = cr.block().begin(); !(it.atEnd()); ++it) {
      QTextFragment currentFragment = it.fragment();
      if (currentFragment.isValid()) {
        auto aa = currentFragment.charFormat().anchorHref();

        if (!aa.isEmpty()) {
          auto xx = 5;
        }
        else if (currentFragment.charFormat().isImageFormat()) {
          aa = currentFragment.charFormat().toImageFormat().name();
        }
      }
    }
  }
  */

  if (!m_neededResources.isEmpty()) {
    QTimer::singleShot(20, this, &TextBrowserViewer::reloadHtmlDelayed);
  }

  setVerticalScrollBarPosition(0.0);
}

void TextBrowserViewer::setReadabledHtml(const QString& html, const QUrl& base_url) {
  setHtml(html, base_url);
}

void TextBrowserViewer::setHtmlPrivate(const QString& html, const QUrl& base_url) {
  m_currentUrl = base_url;
  m_currentHtml = html;

  QTextBrowser::setHtml(html);
  setZoomFactor(m_zoomFactor);

  emit pageTitleChanged(documentTitle());
  emit pageUrlChanged(base_url);
}

TextBrowserDocument::TextBrowserDocument(TextBrowserViewer* parent) : QTextDocument(parent) {
  m_viewer = parent;
}

QVariant TextBrowserDocument::loadResource(int type, const QUrl& name) {
  return m_viewer->loadOneResource(type, name);
}

void TextBrowserViewer::reloadHtmlDelayed() {
  if (!m_neededResources.isEmpty()) {
    downloadNextNeededResource();
  }
}

void TextBrowserViewer::downloadNextNeededResource() {
  if (m_neededResources.isEmpty()) {
    // Everything is downloaded.
    emit reloadDocument();
  }
  else {
    QUrl res = m_neededResources.takeFirst();

    QMetaObject::invokeMethod(m_resourceDownloader,
                              "manipulateData",
                              Qt::ConnectionType::QueuedConnection,
                              Q_ARG(QString, WebFactory::unescapeHtml(res.toString())),
                              Q_ARG(QNetworkAccessManager::Operation, QNetworkAccessManager::Operation::GetOperation),
                              Q_ARG(QByteArray, {}),
                              Q_ARG(int, 5000));

    /*
m_resourceDownloader.data()->manipulateData(qApp->web()->unescapeHtml(res.toString()),
                                            QNetworkAccessManager::Operation::GetOperation,
                                            {},
                                            5000);
                                                */
  }
}

void TextBrowserViewer::resourceDownloaded(const QUrl& url,
                                           QNetworkReply::NetworkError status,
                                           int http_code,
                                           const QByteArray& contents) {
  Q_UNUSED(http_code)
  if (!m_loadedResources.contains(url)) {
    m_loadedResources.insert(url, QMap<int, QByteArray>());
  }

  QMap<int, QByteArray>& resource_data_all_sizes = m_loadedResources[url];

  resource_data_all_sizes.clear();

  if (status == QNetworkReply::NetworkError::NoError) {
    resource_data_all_sizes.insert(0, contents);
  }
  else {
    resource_data_all_sizes.insert(0, {});
  }

  downloadNextNeededResource();
}

PreparedHtml TextBrowserViewer::prepareLegacyHtmlForMessage(const QList<Message>& messages,
                                                            RootItem* selected_item) const {
  PreparedHtml html;
  bool acc_displays_enclosures =
    selected_item == nullptr || selected_item->getParentServiceRoot()->displaysEnclosures();

  for (const Message& message : messages) {
    bool is_plain = !TextFactory::couldBeHtml(message.m_contents);

    // Add title.
    if (!message.m_url.isEmpty()) {
      html.m_html += QSL("<h2 align=\"center\"><a href=\"%2\">%1</a></h2>").arg(message.m_title, message.m_url);
    }
    else {
      html.m_html += QSL("<h2 align=\"center\">%1</h2>").arg(message.m_title);
    }

    // Start contents.
    html.m_html += QSL("<div>");

    // Add links to enclosures.
    if (acc_displays_enclosures) {
      for (const Enclosure& enc : message.m_enclosures) {
        html.m_html += QSL("[<a href=\"%1\">%2</a>]").arg(enc.m_url, enc.m_mimeType);
      }
    }

    // Display enclosures which are pictures if user has it enabled.
    auto first_enc_break_added = false;

    if (acc_displays_enclosures &&
        qApp->settings()->value(GROUP(Messages), SETTING(Messages::DisplayEnclosuresInMessage)).toBool()) {
      for (const Enclosure& enc : message.m_enclosures) {
        if (enc.m_mimeType.startsWith(QSL("image/"))) {
          if (!first_enc_break_added) {
            html.m_html += QSL("<br/>");
            first_enc_break_added = true;
          }

          html.m_html += QSL("<img src=\"%1\" /><br/>").arg(enc.m_url);
        }
      }
    }

    // Append actual contents of article and convert to HTML if needed.
    html.m_html += is_plain ? Qt::convertFromPlainText(message.m_contents, Qt::WhiteSpaceMode::WhiteSpaceNormal)
                            : message.m_contents;

    static QRegularExpression img_tag_rgx(QSL("\\<img[^\\>]*src\\s*=\\s*[\"\']([^\"\']*)[\"\'][^\\>]*\\>"),
                                          QRegularExpression::PatternOption::CaseInsensitiveOption);

    // Extract all images links from article to be appended to end of article.
    QRegularExpressionMatchIterator i = img_tag_rgx.globalMatch(html.m_html);
    QString pictures_html;

    while (i.hasNext()) {
      QRegularExpressionMatch match = i.next();
      auto captured_url = match.captured(1);

      pictures_html += QSL("<br/>[%1] <a href=\"%2\">%2</a>").arg(tr("image"), captured_url);
    }

    // Make alla images clickable as links and also resize them if user has it setup.
    auto forced_img_size =
      qApp->settings()->value(GROUP(Messages), SETTING(Messages::LimitArticleImagesHeight)).toInt();

    // Fixup all "img" tags.
    html.m_html = html.m_html.replace(img_tag_rgx,
                                      QSL("<a href=\"\\1\"><img height=\"%1\" src=\"\\1\" /></a>")
                                        .arg(forced_img_size <= 0 ? QString() : QString::number(forced_img_size)));

    // Append generated list of images.
    html.m_html += pictures_html;
  }

  // Close contents.
  html.m_html += QSL("</div>");

  QString base_url;
  auto* feed = selected_item->getParentServiceRoot()
                 ->getItemFromSubTree([messages](const RootItem* it) {
                   return it->kind() == RootItem::Kind::Feed && it->customId() == messages.at(0).m_feedId;
                 })
                 ->toFeed();

  if (feed != nullptr) {
    QUrl url(NetworkFactory::sanitizeUrl(feed->source()));

    if (url.isValid()) {
      base_url = url.scheme() + QSL("://") + url.host();
    }
  }

  html.m_baseUrl = base_url;

  return html;
}

bool TextBrowserViewer::resourcesEnabled() const {
  return m_resourcesEnabled;
}

void TextBrowserViewer::setResourcesEnabled(bool enabled) {
  m_resourcesEnabled = enabled;
}

ContextMenuData TextBrowserViewer::provideContextMenuData(QContextMenuEvent* event) const {
  ContextMenuData c;

  QString anchor = anchorAt(event->pos());

  if (!anchor.isEmpty()) {
    c.m_linkUrl = anchor;
  }

  return c;
}
