// For license of this file, see <project-root-folder>/LICENSE.md.

#ifndef NETWORKPROXYDETAILS_H
#define NETWORKPROXYDETAILS_H

#include <QNetworkProxy>
#include <QWidget>

namespace Ui {
  class NetworkProxyDetails;
}

class RSSGUARD_DLLSPEC NetworkProxyDetails : public QWidget {
    Q_OBJECT

  public:
    explicit NetworkProxyDetails(bool account_wide_setting, QWidget* parent = nullptr);
    virtual ~NetworkProxyDetails();

    QNetworkProxy proxy() const;
    void setProxy(const QNetworkProxy& proxy);

  signals:
    void changed();

  private slots:
    void onProxyTypeChanged(int index);

  private:
    QScopedPointer<Ui::NetworkProxyDetails> m_ui;
};

#endif // NETWORKPROXYDETAILS_H
