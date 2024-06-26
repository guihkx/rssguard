// For license of this file, see <project-root-folder>/LICENSE.md.

#ifndef GREADERENTRYPOINT_H
#define GREADERENTRYPOINT_H

#include <librssguard/services/abstract/serviceentrypoint.h>

class GreaderEntryPoint : public QObject, public ServiceEntryPoint {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "io.github.martinrotter.rssguard.greader" FILE "plugin.json")
    Q_INTERFACES(ServiceEntryPoint)

  public:
    explicit GreaderEntryPoint(QObject* parent = nullptr);
    virtual ~GreaderEntryPoint();

    virtual ServiceRoot* createNewRoot() const;
    virtual QList<ServiceRoot*> initializeSubtree() const;
    virtual QString name() const;
    virtual QString code() const;
    virtual QString description() const;
    virtual QString author() const;
    virtual QIcon icon() const;
};

#endif // GREADERENTRYPOINT_H
