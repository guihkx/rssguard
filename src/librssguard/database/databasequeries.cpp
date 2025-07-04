// For license of this file, see <project-root-folder>/LICENSE.md.

#include "database/databasequeries.h"

#include "3rd-party/boolinq/boolinq.h"
#include "definitions/globals.h"
#include "exceptions/applicationexception.h"
#include "miscellaneous/application.h"
#include "miscellaneous/iconfactory.h"
#include "miscellaneous/settings.h"
#include "services/abstract/category.h"

#include <QSqlDriver>
#include <QUrl>
#include <QVariant>

QMap<int, QString> DatabaseQueries::messageTableAttributes(bool only_msg_table, bool is_sqlite) {
  QMap<int, QString> field_names;

  field_names[MSG_DB_ID_INDEX] = QSL("Messages.id");
  field_names[MSG_DB_READ_INDEX] = QSL("Messages.is_read");
  field_names[MSG_DB_IMPORTANT_INDEX] = QSL("Messages.is_important");
  field_names[MSG_DB_DELETED_INDEX] = QSL("Messages.is_deleted");
  field_names[MSG_DB_PDELETED_INDEX] = QSL("Messages.is_pdeleted");
  field_names[MSG_DB_FEED_CUSTOM_ID_INDEX] = QSL("Messages.feed");
  field_names[MSG_DB_TITLE_INDEX] = QSL("Messages.title");
  field_names[MSG_DB_URL_INDEX] = QSL("Messages.url");
  field_names[MSG_DB_AUTHOR_INDEX] = QSL("Messages.author");
  field_names[MSG_DB_DCREATED_INDEX] = QSL("Messages.date_created");
  field_names[MSG_DB_CONTENTS_INDEX] = QSL("Messages.contents");
  field_names[MSG_DB_ENCLOSURES_INDEX] = QSL("Messages.enclosures");
  field_names[MSG_DB_SCORE_INDEX] = QSL("Messages.score");
  field_names[MSG_DB_ACCOUNT_ID_INDEX] = QSL("Messages.account_id");
  field_names[MSG_DB_CUSTOM_ID_INDEX] = QSL("Messages.custom_id");
  field_names[MSG_DB_CUSTOM_HASH_INDEX] = QSL("Messages.custom_hash");
  field_names[MSG_DB_FEED_TITLE_INDEX] = only_msg_table ? QSL("Messages.feed") : QSL("Feeds.title");
  field_names[MSG_DB_FEED_IS_RTL_INDEX] = only_msg_table ? QSL("0") : QSL("Feeds.is_rtl");
  field_names[MSG_DB_HAS_ENCLOSURES] = QSL("CASE WHEN LENGTH(Messages.enclosures) > 10 "
                                           "THEN 'true' "
                                           "ELSE 'false' "
                                           "END AS has_enclosures");

  if (is_sqlite) {
    field_names[MSG_DB_LABELS] = QSL("(SELECT GROUP_CONCAT(Labels.name) FROM Labels WHERE Messages.labels LIKE '%.' || "
                                     "Labels.custom_id || '.%') as msg_labels");
  }
  else {
    field_names[MSG_DB_LABELS] =
      QSL("(SELECT GROUP_CONCAT(Labels.name) FROM Labels WHERE Messages.labels LIKE CONCAT('%.', "
          "Labels.custom_id, '.%')) as msg_labels");
  }

  field_names[MSG_DB_LABELS_IDS] = QSL("Messages.labels");

  // TODO: zpomaluje zobrazeni seznamu zprav
  /*
  field_names[MSG_DB_LABELS] =
    QSL("(SELECT GROUP_CONCAT(Labels.name) FROM Labels WHERE Labels.custom_id IN (SELECT "
        "LabelsInMessages.label FROM LabelsInMessages WHERE LabelsInMessages.account_id = "
        "Messages.account_id AND LabelsInMessages.message = Messages.custom_id)) as msg_labels");
        */

  return field_names;
}

QString DatabaseQueries::serializeCustomData(const QVariantHash& data) {
  if (!data.isEmpty()) {
    return QString::fromUtf8(QJsonDocument::fromVariant(data).toJson(QJsonDocument::JsonFormat::Indented));
  }
  else {
    return QString();
  }
}

QVariantHash DatabaseQueries::deserializeCustomData(const QString& data) {
  if (data.isEmpty()) {
    return QVariantHash();
  }
  else {
    auto json = QJsonDocument::fromJson(data.toUtf8());

    return json.object().toVariantHash();
  }
}

bool DatabaseQueries::isLabelAssignedToMessage(const QSqlDatabase& db, Label* label, const Message& msg) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT COUNT(*) FROM Messages "
                "WHERE "
                "  Messages.labels LIKE :label AND "
                "  Messages.custom_id = :message AND "
                "  account_id = :account_id;"));
  q.bindValue(QSL(":label"), QSL("%.%1.%").arg(label->customId()));
  q.bindValue(QSL(":message"), msg.m_customId);
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());

  q.exec() && q.next();

  return q.record().value(0).toInt() > 0;
}

bool DatabaseQueries::deassignLabelFromMessage(const QSqlDatabase& db, Label* label, const Message& msg) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages "
                "SET labels = REPLACE(Messages.labels, :label, \".\") "
                "WHERE Messages.custom_id = :message AND account_id = :account_id;"));
  q.bindValue(QSL(":label"), QSL(".%1.").arg(label->customId()));
  q.bindValue(QSL(":message"), msg.m_customId.isEmpty() ? QString::number(msg.m_id) : msg.m_customId);
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());

  return q.exec();
}

bool DatabaseQueries::assignLabelToMessage(const QSqlDatabase& db, Label* label, const Message& msg) {
  deassignLabelFromMessage(db, label, msg);

  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (db.driverName() == QSL(APP_DB_MYSQL_DRIVER)) {
    q.prepare(QSL("UPDATE Messages "
                  "SET labels = CONCAT(Messages.labels, :label) "
                  "WHERE Messages.custom_id = :message AND account_id = :account_id;"));
  }
  else {
    q.prepare(QSL("UPDATE Messages "
                  "SET labels = Messages.labels || :label "
                  "WHERE Messages.custom_id = :message AND account_id = :account_id;"));
  }

  q.bindValue(QSL(":label"), QSL("%1.").arg(label->customId()));
  q.bindValue(QSL(":message"), msg.m_customId.isEmpty() ? QString::number(msg.m_id) : msg.m_customId);
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());

  return q.exec();
}

bool DatabaseQueries::setLabelsForMessage(const QSqlDatabase& db, const QList<Label*>& labels, const Message& msg) {
  QSqlQuery q(db);

  auto std_lbls = boolinq::from(labels)
                    .select([](Label* lbl) {
                      return lbl->customId();
                    })
                    .toStdList();

  QStringList lbls = FROM_STD_LIST(QStringList, std_lbls);
  QString lblss = QSL(".") + lbls.join('.') + QSL(".");

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages "
                "SET labels = :labels "
                "WHERE Messages.custom_id = :message AND account_id = :account_id;"));
  q.bindValue(QSL(":labels"), lblss);
  q.bindValue(QSL(":message"), msg.m_customId.isEmpty() ? QString::number(msg.m_id) : msg.m_customId);
  q.bindValue(QSL(":account_id"), msg.m_accountId);

  return q.exec();
}

QList<Label*> DatabaseQueries::getLabelsForAccount(const QSqlDatabase& db, int account_id) {
  QList<Label*> labels;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT * FROM Labels WHERE account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      Label* lbl = new Label(q.value(QSL("name")).toString(), QColor(q.value(QSL("color")).toString()));

      lbl->setId(q.value(QSL("id")).toInt());
      lbl->setCustomId(q.value(QSL("custom_id")).toString());

      labels << lbl;
    }
  }

  return labels;
}

QList<Label*> DatabaseQueries::getLabelsForMessage(const QSqlDatabase& db,
                                                   const Message& msg,
                                                   const QList<Label*>& installed_labels) {
  QList<Label*> labels;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT labels FROM Messages WHERE custom_id = :message AND account_id = :account_id;"));

  q.bindValue(QSL(":account_id"), msg.m_accountId);
  q.bindValue(QSL(":message"), msg.m_customId.isEmpty() ? QString::number(msg.m_id) : msg.m_customId);

  if (q.exec() && q.next()) {
    auto label_ids = q.value(0).toString().split('.',
#if QT_VERSION >= 0x050F00 // Qt >= 5.15.0
                                                 Qt::SplitBehaviorFlags::SkipEmptyParts);
#else
                                                 QString::SplitBehavior::SkipEmptyParts);
#endif

    auto iter = boolinq::from(installed_labels);

    for (const QString& lbl_id : label_ids) {
      Label* candidate_label = iter.firstOrDefault([&](const Label* lbl) {
        return lbl->customId() == lbl_id;
      });

      if (candidate_label != nullptr) {
        labels << candidate_label;
      }
    }
  }

  return labels;
}

bool DatabaseQueries::updateLabel(const QSqlDatabase& db, Label* label) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Labels SET name = :name, color = :color "
                "WHERE id = :id AND account_id = :account_id;"));
  q.bindValue(QSL(":name"), label->title());
  q.bindValue(QSL(":color"), label->color().name());
  q.bindValue(QSL(":id"), label->id());
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());

  return q.exec();
}

bool DatabaseQueries::deleteLabel(const QSqlDatabase& db, Label* label) {
  // NOTE: All dependecies are done via SQL foreign cascaded keys, so no
  // extra removals are needed.
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Labels WHERE id = :id AND account_id = :account_id;"));
  q.bindValue(QSL(":id"), label->id());
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());

  if (q.exec()) {
    q.prepare(QSL("UPDATE Messages "
                  "SET labels = REPLACE(Messages.labels, :label, \".\") "
                  "WHERE account_id = :account_id;"));
    q.bindValue(QSL(":label"), QSL(".%1.").arg(label->customId()));
    q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());

    return q.exec();
  }
  else {
    return false;
  }
}

bool DatabaseQueries::createLabel(const QSqlDatabase& db, Label* label, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("INSERT INTO Labels (name, color, custom_id, account_id) "
                "VALUES (:name, :color, :custom_id, :account_id);"));
  q.bindValue(QSL(":name"), label->title());
  q.bindValue(QSL(":color"), label->color().name());
  q.bindValue(QSL(":custom_id"), label->customId());
  q.bindValue(QSL(":account_id"), account_id);

  auto res = q.exec();

  if (res && q.lastInsertId().isValid()) {
    label->setId(q.lastInsertId().toInt());

    // NOTE: This custom ID in this object will be probably
    // overwritten in online-synchronized labels.
    if (label->customId().isEmpty()) {
      label->setCustomId(QString::number(label->id()));
    }
  }

  // Fixup missing custom IDs.
  q.prepare(QSL("UPDATE Labels SET custom_id = id WHERE custom_id IS NULL OR custom_id = '';"));

  return q.exec() && res;
}

void DatabaseQueries::updateProbe(const QSqlDatabase& db, Search* probe) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Probes SET name = :name, fltr = :fltr, color = :color "
                "WHERE id = :id AND account_id = :account_id;"));
  q.bindValue(QSL(":name"), probe->title());
  q.bindValue(QSL(":fltr"), probe->filter());
  q.bindValue(QSL(":color"), probe->color().name());
  q.bindValue(QSL(":id"), probe->id());
  q.bindValue(QSL(":account_id"), probe->getParentServiceRoot()->accountId());

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
}

void DatabaseQueries::createProbe(const QSqlDatabase& db, Search* probe, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("INSERT INTO Probes (name, color, fltr, account_id) "
                "VALUES (:name, :color, :fltr, :account_id);"));
  q.bindValue(QSL(":name"), probe->title());
  q.bindValue(QSL(":fltr"), probe->filter());
  q.bindValue(QSL(":color"), probe->color().name());
  q.bindValue(QSL(":account_id"), account_id);

  auto res = q.exec();

  if (res && q.lastInsertId().isValid()) {
    probe->setId(q.lastInsertId().toInt());
    probe->setCustomId(QString::number(probe->id()));
  }
  else {
    throw ApplicationException(q.lastError().text());
  }
}

QList<Search*> DatabaseQueries::getProbesForAccount(const QSqlDatabase& db, int account_id) {
  QList<Search*> probes;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT * FROM Probes WHERE account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      Search* prob = new Search(q.value(QSL("name")).toString(),
                                q.value(QSL("fltr")).toString(),
                                QColor(q.value(QSL("color")).toString()));

      prob->setId(q.value(QSL("id")).toInt());
      prob->setCustomId(QString::number(prob->id()));

      probes << prob;
    }
  }
  else {
    throw ApplicationException(q.lastError().text());
  }

  return probes;
}

void DatabaseQueries::deleteProbe(const QSqlDatabase& db, Search* probe) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Probes WHERE id = :id AND account_id = :account_id;"));
  q.bindValue(QSL(":id"), probe->id());
  q.bindValue(QSL(":account_id"), probe->getParentServiceRoot()->accountId());

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
}

bool DatabaseQueries::markLabelledMessagesReadUnread(const QSqlDatabase& db, Label* label, RootItem::ReadStatus read) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_read = :read "
                "WHERE "
                "    is_deleted = 0 AND "
                "    is_pdeleted = 0 AND "
                "    account_id = :account_id AND "
                "    labels LIKE :label;"));
  q.bindValue(QSL(":read"), read == RootItem::ReadStatus::Read ? 1 : 0);
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":label"), QSL("%.%1.%").arg(label->customId()));

  return q.exec();
}

bool DatabaseQueries::markImportantMessagesReadUnread(const QSqlDatabase& db,
                                                      int account_id,
                                                      RootItem::ReadStatus read) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_read = :read "
                "WHERE is_important = 1 AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":read"), read == RootItem::ReadStatus::Read ? 1 : 0);
  q.bindValue(QSL(":account_id"), account_id);
  return q.exec();
}

bool DatabaseQueries::markUnreadMessagesRead(const QSqlDatabase& db, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_read = :read "
                "WHERE is_read = 0 AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":read"), 1);
  q.bindValue(QSL(":account_id"), account_id);
  return q.exec();
}

bool DatabaseQueries::markMessagesReadUnread(const QSqlDatabase& db,
                                             const QStringList& ids,
                                             RootItem::ReadStatus read) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  return q.exec(QString(QSL("UPDATE Messages SET is_read = %2 WHERE id IN (%1);"))
                  .arg(ids.join(QSL(", ")), read == RootItem::ReadStatus::Read ? QSL("1") : QSL("0")));
}

void DatabaseQueries::markMessagesReadUnreadImportant(const QSqlDatabase& db,
                                                      int account_id,
                                                      const QStringList& custom_ids,
                                                      RootItem::ReadStatus read,
                                                      RootItem::Importance important) {
  auto stringed_ids = boolinq::from(custom_ids.begin(), custom_ids.end())
                        .select([](const QString& id) {
                          return QSL("'%1'").arg(id);
                        })
                        .toStdList();

  QStringList textual_ids = FROM_STD_LIST(QStringList, stringed_ids);
  QSqlQuery q(db);
  QStringList setters;

  if (read != RootItem::ReadStatus::Unknown) {
    setters.append(QSL("is_read = :read"));
  }

  if (important != RootItem::Importance::Unknown) {
    setters.append(QSL("is_important = :important"));
  }

  q.setForwardOnly(true);

  QString statement = QSL("UPDATE Messages SET %1 "
                          "  WHERE account_id = :account_id AND custom_id in (%2);")
                        .arg(setters.join(", "), textual_ids.join(", "));

  if (!q.prepare(statement)) {
    throw ApplicationException(q.lastError().text());
  }

  q.bindValue(QSL(":read"), int(read));
  q.bindValue(QSL(":important"), int(important));
  q.bindValue(QSL(":account_id"), account_id);

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
}

bool DatabaseQueries::markMessageImportant(const QSqlDatabase& db, int id, RootItem::Importance importance) {
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (!q.prepare(QSL("UPDATE Messages SET is_important = :important WHERE id = :id;"))) {
    qWarningNN << LOGSEC_DB << "Query preparation failed for message importance switch.";
    return false;
  }

  q.bindValue(QSL(":id"), id);
  q.bindValue(QSL(":important"), (int)importance);

  // Commit changes.
  return q.exec();
}

bool DatabaseQueries::markFeedsReadUnread(const QSqlDatabase& db,
                                          const QStringList& ids,
                                          int account_id,
                                          RootItem::ReadStatus read) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_read = :read "
                "WHERE feed IN (%1) AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;")
              .arg(ids.join(QSL(", "))));
  q.bindValue(QSL(":read"), read == RootItem::ReadStatus::Read ? 1 : 0);
  q.bindValue(QSL(":account_id"), account_id);
  return q.exec();
}

bool DatabaseQueries::markBinReadUnread(const QSqlDatabase& db, int account_id, RootItem::ReadStatus read) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_read = :read "
                "WHERE is_deleted = 1 AND is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":read"), read == RootItem::ReadStatus::Read ? 1 : 0);
  q.bindValue(QSL(":account_id"), account_id);
  return q.exec();
}

bool DatabaseQueries::markAccountReadUnread(const QSqlDatabase& db, int account_id, RootItem::ReadStatus read) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_read = :read WHERE is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":read"), read == RootItem::ReadStatus::Read ? 1 : 0);
  return q.exec();
}

bool DatabaseQueries::switchMessagesImportance(const QSqlDatabase& db, const QStringList& ids) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  return q.exec(QSL("UPDATE Messages SET is_important = NOT is_important WHERE id IN (%1);").arg(ids.join(QSL(", "))));
}

bool DatabaseQueries::permanentlyDeleteMessages(const QSqlDatabase& db, const QStringList& ids) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  return q.exec(QSL("UPDATE Messages SET is_pdeleted = 1 WHERE id IN (%1);").arg(ids.join(QSL(", "))));
}

bool DatabaseQueries::deleteOrRestoreMessagesToFromBin(const QSqlDatabase& db, const QStringList& ids, bool deleted) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  return q.exec(QSL("UPDATE Messages SET is_deleted = %2, is_pdeleted = %3 WHERE id IN (%1);")
                  .arg(ids.join(QSL(", ")), QString::number(deleted ? 1 : 0), QString::number(0)));
}

bool DatabaseQueries::restoreBin(const QSqlDatabase& db, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_deleted = 0 "
                "WHERE is_deleted = 1 AND is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);
  return q.exec();
}

bool DatabaseQueries::removeUnwantedArticlesFromFeed(const QSqlDatabase& db,
                                                     const Feed* feed,
                                                     const Feed::ArticleIgnoreLimit& feed_setup,
                                                     const Feed::ArticleIgnoreLimit& app_setup) {
  // Feed setup has higher preference.
  int amount_to_keep =
    feed_setup.m_customizeLimitting ? feed_setup.m_keepCountOfArticles : app_setup.m_keepCountOfArticles;
  bool dont_remove_unread =
    feed_setup.m_customizeLimitting ? feed_setup.m_doNotRemoveUnread : app_setup.m_doNotRemoveUnread;
  bool dont_remove_starred =
    feed_setup.m_customizeLimitting ? feed_setup.m_doNotRemoveStarred : app_setup.m_doNotRemoveStarred;
  bool recycle_dont_purge =
    feed_setup.m_customizeLimitting ? feed_setup.m_moveToBinDontPurge : app_setup.m_moveToBinDontPurge;

  if (amount_to_keep <= 0) {
    // No articles will be removed, quitting.
    return false;
  }

  // We find datetime stamp of oldest article which will be NOT moved/removed.
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT Messages.date_created "
                "FROM Messages "
                "WHERE "
                "  Messages.account_id = :account_id AND "
                "  Messages.feed = :feed AND "
                "  Messages.is_deleted = 0 AND "
                "  Messages.is_pdeleted = 0 "
                "ORDER BY Messages.date_created DESC "
                "LIMIT 1 OFFSET :offset;"));

  q.bindValue(QSL(":offset"), amount_to_keep - 1);
  q.bindValue(QSL(":feed"), feed->customId());
  q.bindValue(QSL(":account_id"), feed->getParentServiceRoot()->accountId());

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }

  if (!q.next()) {
    return false;
  }

  qlonglong last_kept_stamp = q.value(0).toLongLong();

  if (recycle_dont_purge) {
    // We mark all older articles as deleted.
    q.prepare(QSL("UPDATE Messages "
                  "SET is_deleted = 1 "
                  "WHERE "
                  "  Messages.account_id = :account_id AND "
                  "  Messages.feed = :feed AND "
                  "  Messages.is_deleted = 0 AND "
                  "  Messages.is_pdeleted = 0 AND "
                  "  Messages.is_important != :is_important AND "
                  "  Messages.is_read != :is_read AND "
                  "  Messages.date_created < :stamp"));
  }
  else {
    // We purge all older articles.
    q.prepare(QSL("DELETE FROM Messages "
                  "WHERE "
                  "  Messages.account_id = :account_id AND "
                  "  Messages.feed = :feed AND "
                  "  (Messages.is_deleted = 1 OR Messages.is_important != :is_important) AND "
                  "  (Messages.is_deleted = 1 OR Messages.is_read != :is_read) AND "
                  "  Messages.date_created < :stamp"));
  }

  q.bindValue(QSL(":is_important"), dont_remove_starred ? 1 : 2);
  q.bindValue(QSL(":is_read"), dont_remove_unread ? 0 : 2);
  q.bindValue(QSL(":feed"), feed->customId());
  q.bindValue(QSL(":stamp"), last_kept_stamp);
  q.bindValue(QSL(":account_id"), feed->getParentServiceRoot()->accountId());

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }

  int rows_deleted = q.numRowsAffected();

  qDebugNN << LOGSEC_DB << "Feed cleanup has recycled/purged" << NONQUOTE_W_SPACE(rows_deleted)
           << "old articles from feed" << QUOTE_W_SPACE_DOT(feed->customId());

  return rows_deleted > 0;
}

bool DatabaseQueries::purgeFeedArticles(const QSqlDatabase& database, const QList<Feed*>& feeds) {
  QSqlQuery q(database);

  auto feed_clauses = boolinq::from(feeds)
                        .select([](Feed* feed) {
                          return QSL("("
                                     "Messages.feed = '%1' AND "
                                     "Messages.account_id = %2 AND "
                                     "Messages.is_important = 0"
                                     ")")
                            .arg(feed->customId(), QString::number(feed->getParentServiceRoot()->accountId()));
                        })
                        .toStdList();

  qDebugNN << feed_clauses;

  QStringList feed_str_clauses = FROM_STD_LIST(QStringList, feed_clauses);
  QString feed_clause = feed_str_clauses.join(QSL(" OR "));

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Messages WHERE %1;").arg(feed_clause));

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
  else {
    return q.numRowsAffected() > 0;
  }
}

bool DatabaseQueries::purgeMessage(const QSqlDatabase& db, int message_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Messages WHERE id = :id;"));
  q.bindValue(QSL(":id"), message_id);

  return q.exec();
}

bool DatabaseQueries::purgeImportantMessages(const QSqlDatabase& db) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Messages WHERE is_important = 1 AND is_deleted = :is_deleted;"));

  // Remove only messages which are NOT in recycle bin.
  q.bindValue(QSL(":is_deleted"), 0);

  return q.exec();
}

bool DatabaseQueries::purgeReadMessages(const QSqlDatabase& db) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Messages "
                "WHERE is_important = :is_important AND is_deleted = :is_deleted AND is_read = :is_read;"));
  q.bindValue(QSL(":is_read"), 1);

  // Remove only messages which are NOT in recycle bin.
  q.bindValue(QSL(":is_deleted"), 0);

  // Remove only messages which are NOT starred.
  q.bindValue(QSL(":is_important"), 0);

  return q.exec();
}

bool DatabaseQueries::purgeOldMessages(const QSqlDatabase& db, int older_than_days) {
  QSqlQuery q(db);
  const qint64 since_epoch = older_than_days == 0
                               ? QDateTime::currentDateTimeUtc().addYears(10).toMSecsSinceEpoch()
                               : QDateTime::currentDateTimeUtc().addDays(-older_than_days).toMSecsSinceEpoch();

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Messages WHERE is_important = :is_important AND date_created < :date_created;"));
  q.bindValue(QSL(":date_created"), since_epoch);

  // Remove only messages which are NOT starred.
  q.bindValue(QSL(":is_important"), 0);
  return q.exec();
}

bool DatabaseQueries::purgeRecycleBin(const QSqlDatabase& db) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Messages WHERE is_important = :is_important AND is_deleted = :is_deleted;"));
  q.bindValue(QSL(":is_deleted"), 1);

  // Remove only messages which are NOT starred.
  q.bindValue(QSL(":is_important"), 0);
  return q.exec();
}

QMap<QString, ArticleCounts> DatabaseQueries::getMessageCountsForCategory(const QSqlDatabase& db,
                                                                          const QString& custom_id,
                                                                          int account_id,
                                                                          bool include_total_counts,
                                                                          bool* ok) {
  QMap<QString, ArticleCounts> counts;
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (include_total_counts) {
    q.prepare(QSL("SELECT feed, SUM((is_read + 1) % 2), COUNT(*) FROM Messages "
                  "WHERE feed IN (SELECT custom_id FROM Feeds WHERE category = :category AND account_id = :account_id) "
                  "AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id "
                  "GROUP BY feed;"));
  }
  else {
    q.prepare(QSL("SELECT feed, SUM((is_read + 1) % 2) FROM Messages "
                  "WHERE feed IN (SELECT custom_id FROM Feeds WHERE category = :category AND account_id = :account_id) "
                  "AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id "
                  "GROUP BY feed;"));
  }

  q.bindValue(QSL(":category"), custom_id);
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      QString feed_custom_id = q.value(0).toString();
      ArticleCounts ac;

      ac.m_unread = q.value(1).toInt();

      if (include_total_counts) {
        ac.m_total = q.value(2).toInt();
      }

      counts.insert(feed_custom_id, ac);
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return counts;
}

QMap<QString, ArticleCounts> DatabaseQueries::getMessageCountsForAccount(const QSqlDatabase& db,
                                                                         int account_id,
                                                                         bool include_total_counts,
                                                                         bool* ok) {
  QMap<QString, ArticleCounts> counts;
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (include_total_counts) {
    q.prepare(QSL("SELECT feed, SUM((is_read + 1) % 2), COUNT(*) FROM Messages "
                  "WHERE is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id "
                  "GROUP BY feed;"));
  }
  else {
    q.prepare(QSL("SELECT feed, SUM((is_read + 1) % 2) FROM Messages "
                  "WHERE is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id "
                  "GROUP BY feed;"));
  }

  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      QString feed_id = q.value(0).toString();
      ArticleCounts ac;

      ac.m_unread = q.value(1).toInt();

      if (include_total_counts) {
        ac.m_total = q.value(2).toInt();
      }

      counts.insert(feed_id, ac);
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return counts;
}

ArticleCounts DatabaseQueries::getMessageCountsForFeed(const QSqlDatabase& db,
                                                       const QString& feed_custom_id,
                                                       int account_id,
                                                       bool* ok) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT COUNT(*), SUM(is_read) FROM Messages "
                "WHERE feed = :feed AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;"));

  q.bindValue(QSL(":feed"), feed_custom_id);
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec() && q.next()) {
    if (ok != nullptr) {
      *ok = true;
    }

    ArticleCounts ac;

    ac.m_total = q.value(0).toInt();
    ac.m_unread = ac.m_total - q.value(1).toInt();

    return ac;
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }

    return {};
  }
}

ArticleCounts DatabaseQueries::getMessageCountsForLabel(const QSqlDatabase& db,
                                                        Label* label,
                                                        int account_id,
                                                        bool* ok) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT COUNT(*), SUM(is_read) FROM Messages "
                "WHERE "
                "  is_deleted = 0 AND "
                "  is_pdeleted = 0 AND "
                "  account_id = :account_id AND "
                "  labels LIKE :label;"));

  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":label"), QSL("%.%1.%").arg(label->customId()));

  if (q.exec() && q.next()) {
    if (ok != nullptr) {
      *ok = true;
    }

    ArticleCounts ac;

    ac.m_total = q.value(0).toInt();
    ac.m_unread = ac.m_total - q.value(1).toInt();

    return ac;
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }

    return {};
  }
}

ArticleCounts DatabaseQueries::getMessageCountsForProbe(const QSqlDatabase& db, Search* probe, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT COUNT(*), SUM(is_read) FROM Messages "
                "WHERE "
                "  is_deleted = 0 AND "
                "  is_pdeleted = 0 AND "
                "  account_id = :account_id AND "
                "  (title REGEXP :fltr OR contents REGEXP :fltr);"));

  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":fltr"), probe->filter());

  if (q.exec() && q.next()) {
    ArticleCounts ac;

    ac.m_total = q.value(0).toInt();
    ac.m_unread = ac.m_total - q.value(1).toInt();

    return ac;
  }
  else {
    throw ApplicationException(q.lastError().text());
  }
}

QMap<QString, ArticleCounts> DatabaseQueries::getMessageCountsForAllLabels(const QSqlDatabase& db,
                                                                           int account_id,
                                                                           bool* ok) {
  QMap<QString, ArticleCounts> counts;
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (db.driverName() == QSL(APP_DB_MYSQL_DRIVER)) {
    q.prepare(QSL("SELECT l.custom_id, CONCAT('%.', l.custom_id,'.%') pid, SUM(m.is_read), COUNT(*) FROM Labels l "
                  "INNER JOIN Messages m "
                  "  ON m.account_id = l.account_id AND m.labels LIKE pid "
                  "WHERE "
                  "  m.is_deleted = 0 AND "
                  "  m.is_pdeleted = 0 AND "
                  "  m.account_id = :account_id "
                  "GROUP BY pid;"));
  }
  else {
    q.prepare(QSL("SELECT l.custom_id, ('%.' || l.custom_id || '.%') pid, SUM(m.is_read), COUNT(*) FROM Labels l "
                  "INNER JOIN Messages m "
                  "  ON m.account_id = l.account_id AND m.labels LIKE pid "
                  "WHERE "
                  "  m.is_deleted = 0 AND "
                  "  m.is_pdeleted = 0 AND "
                  "  m.account_id = :account_id "
                  "GROUP BY pid;"));
  }

  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      QString lbl_custom_id = q.value(0).toString();
      ArticleCounts ac;

      ac.m_total = q.value(3).toInt();
      ac.m_unread = ac.m_total - q.value(2).toInt();

      counts.insert(lbl_custom_id, ac);
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return counts;
}

QMap<QString, ArticleCounts> DatabaseQueries::getCountOfAssignedLabelsToMessages(const QSqlDatabase& db,
                                                                                 const QList<Message>& messages,
                                                                                 int account_id,
                                                                                 bool* ok) {
  QMap<QString, ArticleCounts> counts;
  QSqlQuery q(db);

  q.setForwardOnly(true);

  auto msgs_std = boolinq::from(messages)
                    .select([](const Message& msg) {
                      return QSL("m.custom_id = '%1'").arg(msg.m_customId);
                    })
                    .toStdList();

  QStringList msgs_lst = FROM_STD_LIST(QStringList, msgs_std);
  auto msgs = msgs_lst.join(QSL(" OR "));

  if (db.driverName() == QSL(APP_DB_MYSQL_DRIVER)) {
    q.prepare(QSL("SELECT l.custom_id, CONCAT('%.', l.custom_id,'.%') pid, SUM(m.is_read), COUNT(*) FROM Labels l "
                  "INNER JOIN Messages m "
                  "  ON m.account_id = l.account_id AND m.labels LIKE pid "
                  "WHERE "
                  "  m.is_deleted = 0 AND "
                  "  m.is_pdeleted = 0 AND "
                  "  m.account_id = :account_id AND "
                  " (%1) "
                  "GROUP BY pid;")
                .arg(msgs));
  }
  else {
    q.prepare(QSL("SELECT l.custom_id, ('%.' || l.custom_id || '.%') pid, SUM(m.is_read), COUNT(*) FROM Labels l "
                  "INNER JOIN Messages m "
                  "  ON m.account_id = l.account_id AND m.labels LIKE pid "
                  "WHERE "
                  "  m.is_deleted = 0 AND "
                  "  m.is_pdeleted = 0 AND "
                  "  m.account_id = :account_id AND "
                  " (%1) "
                  "GROUP BY pid;")
                .arg(msgs));
  }

  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      QString lbl_custom_id = q.value(0).toString();
      ArticleCounts ac;

      ac.m_total = q.value(3).toInt();
      ac.m_unread = ac.m_total - q.value(2).toInt();

      counts.insert(lbl_custom_id, ac);
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return counts;
}

ArticleCounts DatabaseQueries::getImportantMessageCounts(const QSqlDatabase& db, int account_id, bool* ok) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT COUNT(*), SUM(is_read) FROM Messages "
                "WHERE is_important = 1 AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = "
                ":account_id;"));
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec() && q.next()) {
    if (ok != nullptr) {
      *ok = true;
    }

    ArticleCounts ac;

    ac.m_total = q.value(0).toInt();
    ac.m_unread = ac.m_total - q.value(1).toInt();

    return ac;
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }

    return {};
  }
}

int DatabaseQueries::getUnreadMessageCounts(const QSqlDatabase& db, int account_id, bool* ok) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT COUNT(*) FROM Messages "
                "WHERE is_read = 0 AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;"));

  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec() && q.next()) {
    if (ok != nullptr) {
      *ok = true;
    }

    return q.value(0).toInt();
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }

    return 0;
  }
}

ArticleCounts DatabaseQueries::getMessageCountsForBin(const QSqlDatabase& db, int account_id, bool* ok) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT COUNT(*), SUM(is_read) FROM Messages "
                "WHERE is_deleted = 1 AND is_pdeleted = 0 AND account_id = :account_id;"));

  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec() && q.next()) {
    if (ok != nullptr) {
      *ok = true;
    }

    ArticleCounts ac;

    ac.m_total = q.value(0).toInt();
    ac.m_unread = ac.m_total - q.value(1).toInt();

    return ac;
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }

    return {};
  }
}

QList<Message> DatabaseQueries::getUndeletedMessagesForProbe(const QSqlDatabase& db, const Search* probe) {
  QList<Message> messages;
  QSqlQuery q(db);

  q.prepare(QSL("SELECT %1 "
                "FROM Messages "
                "WHERE "
                "  Messages.is_deleted = 0 AND "
                "  Messages.is_pdeleted = 0 AND "
                "  Messages.account_id = :account_id AND "
                "  (title REGEXP :fltr OR contents REGEXP :fltr);")
              .arg(messageTableAttributes(true, db.driverName() == QSL(APP_DB_SQLITE_DRIVER))
                     .values()
                     .join(QSL(", "))));
  q.bindValue(QSL(":account_id"), probe->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":fltr"), probe->filter());

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }
  }
  else {
    throw ApplicationException(q.lastError().text());
  }

  return messages;
}

QList<Message> DatabaseQueries::getUndeletedMessagesWithLabel(const QSqlDatabase& db, const Label* label, bool* ok) {
  QList<Message> messages;
  QSqlQuery q(db);

  q.prepare(QSL("SELECT %1 "
                "FROM Messages "
                "INNER JOIN Feeds "
                "ON Messages.feed = Feeds.custom_id AND Messages.account_id = :account_id AND Messages.account_id = "
                "Feeds.account_id "
                "WHERE "
                "  Messages.is_deleted = 0 AND "
                "  Messages.is_pdeleted = 0 AND "
                "  Messages.account_id = :account_id AND "
                "  Messages.labels LIKE :label;")
              .arg(messageTableAttributes(false, db.driverName() == QSL(APP_DB_SQLITE_DRIVER))
                     .values()
                     .join(QSL(", "))));
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":label"), QSL("%.%1.%").arg(label->customId()));

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return messages;
}

QList<Message> DatabaseQueries::getUndeletedLabelledMessages(const QSqlDatabase& db, int account_id, bool* ok) {
  QList<Message> messages;
  QSqlQuery q(db);

  q.prepare(QSL("SELECT %1 "
                "FROM Messages "
                "INNER JOIN Feeds "
                "ON Messages.feed = Feeds.custom_id AND Messages.account_id = Feeds.account_id "
                "WHERE "
                "  Messages.is_deleted = 0 AND "
                "  Messages.is_pdeleted = 0 AND "
                "  Messages.account_id = :account_id AND "
                "  LENGTH(Messages.labels) > 2;")
              .arg(messageTableAttributes(false, db.driverName() == QSL(APP_DB_SQLITE_DRIVER))
                     .values()
                     .join(QSL(", "))));
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    auto a = q.lastError().text();

    if (ok != nullptr) {
      *ok = false;
    }
  }

  return messages;
}

QList<Message> DatabaseQueries::getUndeletedImportantMessages(const QSqlDatabase& db, int account_id, bool* ok) {
  QList<Message> messages;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT %1 "
                "FROM Messages "
                "WHERE is_important = 1 AND is_deleted = 0 AND "
                "      is_pdeleted = 0 AND account_id = :account_id;")
              .arg(messageTableAttributes(true, db.driverName() == QSL(APP_DB_SQLITE_DRIVER))
                     .values()
                     .join(QSL(", "))));
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return messages;
}

QList<Message> DatabaseQueries::getUndeletedUnreadMessages(const QSqlDatabase& db, int account_id, bool* ok) {
  QList<Message> messages;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT %1 "
                "FROM Messages "
                "WHERE is_read = 0 AND is_deleted = 0 AND "
                "      is_pdeleted = 0 AND account_id = :account_id;")
              .arg(messageTableAttributes(true, db.driverName() == QSL(APP_DB_SQLITE_DRIVER))
                     .values()
                     .join(QSL(", "))));
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return messages;
}

QList<Message> DatabaseQueries::getArticlesSlice(const QSqlDatabase& db,
                                                 const QString& feed_custom_id,
                                                 int account_id,
                                                 bool newest_first,
                                                 bool unread_only,
                                                 bool starred_only,
                                                 qint64 start_after_article_date,
                                                 int row_offset,
                                                 int row_limit) {
  QList<Message> messages;
  QSqlQuery q(db);
  QString feed_clause = !feed_custom_id.isEmpty() ? QSL("Messages.feed = :feed AND") : QString();
  QString is_read_clause = unread_only ? QSL("Messages.is_read = :is_read AND ") : QString();
  QString is_starred_clause = starred_only ? QSL("Messages.is_important = :is_important AND ") : QString();
  QString account_id_clause = account_id > 0 ? QSL("Messages.account_id = :account_id AND ") : QString();
  QString date_created_clause;

  if (start_after_article_date > 0) {
    if (newest_first) {
      date_created_clause = QSL("Messages.date_created < :date_created AND ");
    }
    else {
      date_created_clause = QSL("Messages.date_created > :date_created AND ");
    }
  }

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT %1 "
                "FROM Messages LEFT JOIN Feeds ON Messages.feed = Feeds.custom_id AND "
                "                                 Messages.account_id = Feeds.account_id "
                "WHERE %3 "
                "      %4 "
                "      %5 "
                "      %6 "
                "      %7 "
                "      Messages.is_deleted = 0 AND "
                "      Messages.is_pdeleted = 0 "
                "ORDER BY Messages.date_created %2 "
                "LIMIT :row_limit OFFSET :row_offset;")
              .arg(messageTableAttributes(false, db.driverName() == QSL(APP_DB_SQLITE_DRIVER)).values().join(QSL(", ")),
                   newest_first ? QSL("DESC") : QSL("ASC"),
                   feed_clause,
                   date_created_clause,
                   account_id_clause,
                   is_read_clause,
                   is_starred_clause));
  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":row_limit"), row_limit);
  q.bindValue(QSL(":row_offset"), row_offset);
  q.bindValue(QSL(":feed"), feed_custom_id);
  q.bindValue(QSL(":is_read"), 0);
  q.bindValue(QSL(":is_important"), 1);
  q.bindValue(QSL(":date_created"), start_after_article_date);

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }
  }
  else {
    throw ApplicationException(q.lastError().driverText() + QSL(" ") + q.lastError().databaseText());
  }

  return messages;
}

QList<Message> DatabaseQueries::getUndeletedMessagesForFeed(const QSqlDatabase& db,
                                                            const QString& feed_custom_id,
                                                            int account_id,
                                                            bool* ok) {
  QList<Message> messages;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT %1 "
                "FROM Messages "
                "WHERE is_deleted = 0 AND is_pdeleted = 0 AND "
                "      feed = :feed AND account_id = :account_id;")
              .arg(messageTableAttributes(true, db.driverName() == QSL(APP_DB_SQLITE_DRIVER))
                     .values()
                     .join(QSL(", "))));
  q.bindValue(QSL(":feed"), feed_custom_id);
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    auto aa = q.lastError().text();

    if (ok != nullptr) {
      *ok = false;
    }
  }

  return messages;
}

QList<Message> DatabaseQueries::getUndeletedMessagesForBin(const QSqlDatabase& db, int account_id, bool* ok) {
  QList<Message> messages;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT %1 "
                "FROM Messages "
                "WHERE is_deleted = 1 AND is_pdeleted = 0 AND account_id = :account_id;")
              .arg(messageTableAttributes(true, db.driverName() == QSL(APP_DB_SQLITE_DRIVER))
                     .values()
                     .join(QSL(", "))));
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return messages;
}

QList<Message> DatabaseQueries::getUndeletedMessagesForAccount(const QSqlDatabase& db, int account_id, bool* ok) {
  QList<Message> messages;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT %1 "
                "FROM Messages "
                "WHERE is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;")
              .arg(messageTableAttributes(true, db.driverName() == QSL(APP_DB_SQLITE_DRIVER))
                     .values()
                     .join(QSL(", "))));
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    while (q.next()) {
      bool decoded;
      Message message = Message::fromSqlRecord(q.record(), &decoded);

      if (decoded) {
        messages.append(message);
      }
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return messages;
}

QStringList DatabaseQueries::bagOfMessages(const QSqlDatabase& db, ServiceRoot::BagOfMessages bag, const Feed* feed) {
  QStringList ids;
  QSqlQuery q(db);
  QString query;

  q.setForwardOnly(true);

  switch (bag) {
    case ServiceRoot::BagOfMessages::Unread:
      query = QSL("is_read = 0");
      break;

    case ServiceRoot::BagOfMessages::Starred:
      query = QSL("is_important = 1");
      break;

    case ServiceRoot::BagOfMessages::Read:
    default:
      query = QSL("is_read = 1");
      break;
  }

  q.prepare(QSL("SELECT custom_id "
                "FROM Messages "
                "WHERE %1 AND feed = :feed AND account_id = :account_id;")
              .arg(query));

  q.bindValue(QSL(":account_id"), feed->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":feed"), feed->customId());
  q.exec();

  while (q.next()) {
    ids.append(q.value(0).toString());
  }

  return ids;
}

QHash<QString, QStringList> DatabaseQueries::bagsOfMessages(const QSqlDatabase& db, const QList<Label*>& labels) {
  QHash<QString, QStringList> ids;
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT custom_id FROM Messages "
                "WHERE "
                "  account_id = :account_id AND "
                "  labels LIKE :label;"));

  for (const Label* lbl : labels) {
    q.bindValue(QSL(":label"), QSL("%.%1.%").arg(lbl->customId()));
    q.bindValue(QSL(":account_id"), lbl->getParentServiceRoot()->accountId());
    q.exec();

    QStringList ids_one_label;

    while (q.next()) {
      ids_one_label.append(q.value(0).toString());
    }

    ids.insert(lbl->customId(), ids_one_label);
  }

  return ids;
}

UpdatedArticles DatabaseQueries::updateMessages(const QSqlDatabase& db,
                                                QList<Message>& messages,
                                                Feed* feed,
                                                bool force_update,
                                                QMutex* db_mutex,
                                                bool* ok) {
  if (messages.isEmpty()) {
    *ok = true;
    return {};
  }

  UpdatedArticles updated_messages;
  int account_id = feed->getParentServiceRoot()->accountId();
  auto feed_custom_id = feed->customId();

  // Prepare queries.
  QSqlQuery query_select_with_url(db);
  QSqlQuery query_select_with_custom_id(db);
  QSqlQuery query_select_with_custom_id_for_feed(db);
  QSqlQuery query_select_with_id(db);
  QSqlQuery query_update(db);

  // Here we have query which will check for existence of the "same" message in given feed.
  // The two message are the "same" if:
  //   1) they belong to the SAME FEED AND,
  //   2) they have same URL AND,
  //   3) they have same AUTHOR AND,
  //   4) they have same TITLE.
  // NOTE: This only applies to messages from standard RSS/ATOM/JSON feeds without ID/GUID.
  query_select_with_url.setForwardOnly(true);
  query_select_with_url.prepare(QSL("SELECT id, date_created, is_read, is_important, contents, feed FROM Messages "
                                    "WHERE feed = :feed AND title = :title AND url = :url AND author = :author AND "
                                    "account_id = :account_id;"));

  // When we have custom ID of the message which is service-specific (synchronized services).
  query_select_with_custom_id.setForwardOnly(true);
  query_select_with_custom_id
    .prepare(QSL("SELECT id, date_created, is_read, is_important, contents, feed, title, author FROM Messages "
                 "WHERE custom_id = :custom_id AND account_id = :account_id;"));

  // We have custom ID of message, but it is feed-specific not service-specific (standard RSS/ATOM/JSON).
  query_select_with_custom_id_for_feed.setForwardOnly(true);
  query_select_with_custom_id_for_feed
    .prepare(QSL("SELECT id, date_created, is_read, is_important, contents, title, author FROM Messages "
                 "WHERE feed = :feed AND custom_id = :custom_id AND account_id = :account_id;"));

  // In some case, messages are already stored in the DB and they all have primary DB ID.
  // This is particularly the case when user runs some message filter manually on existing messages
  // of some feed.
  query_select_with_id.setForwardOnly(true);
  query_select_with_id
    .prepare(QSL("SELECT date_created, is_read, is_important, contents, feed, title, author FROM Messages "
                 "WHERE id = :id AND account_id = :account_id;"));

  // Used to update existing messages.
  query_update.setForwardOnly(true);
  query_update.prepare(QSL("UPDATE Messages "
                           "SET title = :title, is_read = :is_read, is_important = :is_important, is_deleted = "
                           ":is_deleted, url = :url, author = :author, score = :score, date_created = :date_created, "
                           "contents = :contents, enclosures = :enclosures, feed = :feed "
                           "WHERE id = :id;"));

  QVector<Message*> msgs_to_insert;

  for (Message& message : messages) {
    int id_existing_message = -1;
    qint64 date_existing_message = 0;
    bool is_read_existing_message = false;
    bool is_important_existing_message = false;
    QString contents_existing_message;
    QString feed_id_existing_message;
    QString title_existing_message;
    QString author_existing_message;

    QMutexLocker lck(db_mutex);

    if (message.m_id > 0) {
      // We recognize directly existing message.
      // NOTE: Particularly for manual message filter execution.
      query_select_with_id.bindValue(QSL(":id"), message.m_id);
      query_select_with_id.bindValue(QSL(":account_id"), account_id);

      qDebugNN << LOGSEC_DB << "Checking if message with primary ID" << QUOTE_W_SPACE(message.m_id)
               << "is present in DB.";

      if (query_select_with_id.exec() && query_select_with_id.next()) {
        id_existing_message = message.m_id;
        date_existing_message = query_select_with_id.value(0).value<qint64>();
        is_read_existing_message = query_select_with_id.value(1).toBool();
        is_important_existing_message = query_select_with_id.value(2).toBool();
        contents_existing_message = query_select_with_id.value(3).toString();
        feed_id_existing_message = query_select_with_id.value(4).toString();
        title_existing_message = query_select_with_id.value(5).toString();
        author_existing_message = query_select_with_id.value(6).toString();

        qDebugNN << LOGSEC_DB << "Message with direct DB ID is already present in DB and has DB ID"
                 << QUOTE_W_SPACE_DOT(id_existing_message);
      }
      else if (query_select_with_id.lastError().isValid()) {
        qWarningNN << LOGSEC_DB << "Failed to check for existing message in DB via primary ID:"
                   << QUOTE_W_SPACE_DOT(query_select_with_id.lastError().text());
      }

      query_select_with_id.finish();
    }
    else if (message.m_customId.isEmpty()) {
      // We need to recognize existing messages according to URL & AUTHOR & TITLE.
      // NOTE: This concerns articles from RSS/ATOM/JSON which do not
      // provide unique ID/GUID.
      query_select_with_url.bindValue(QSL(":feed"), unnulifyString(feed_custom_id));
      query_select_with_url.bindValue(QSL(":title"), unnulifyString(message.m_title));
      query_select_with_url.bindValue(QSL(":url"), unnulifyString(message.m_url));
      query_select_with_url.bindValue(QSL(":author"), unnulifyString(message.m_author));
      query_select_with_url.bindValue(QSL(":account_id"), account_id);

      qDebugNN << LOGSEC_DB << "Checking if message with title " << QUOTE_NO_SPACE(message.m_title) << ", url "
               << QUOTE_NO_SPACE(message.m_url) << "' and author " << QUOTE_NO_SPACE(message.m_author)
               << " is present in DB.";

      if (query_select_with_url.exec() && query_select_with_url.next()) {
        id_existing_message = query_select_with_url.value(0).toInt();
        date_existing_message = query_select_with_url.value(1).value<qint64>();
        is_read_existing_message = query_select_with_url.value(2).toBool();
        is_important_existing_message = query_select_with_url.value(3).toBool();
        contents_existing_message = query_select_with_url.value(4).toString();
        feed_id_existing_message = query_select_with_url.value(5).toString();
        title_existing_message = unnulifyString(message.m_title);
        author_existing_message = unnulifyString(message.m_author);

        qDebugNN << LOGSEC_DB << "Message with these attributes is already present in DB and has DB ID"
                 << QUOTE_W_SPACE_DOT(id_existing_message);
      }
      else if (query_select_with_url.lastError().isValid()) {
        qWarningNN << LOGSEC_DB << "Failed to check for existing message in DB via URL/TITLE/AUTHOR:"
                   << QUOTE_W_SPACE_DOT(query_select_with_url.lastError().text());
      }

      query_select_with_url.finish();
    }
    else {
      // We can recognize existing messages via their custom ID.
      if (feed->getParentServiceRoot()->isSyncable()) {
        // Custom IDs are service-wide.
        // NOTE: This concerns messages from custom accounts, like TT-RSS or Nextcloud News.
        query_select_with_custom_id.bindValue(QSL(":account_id"), account_id);
        query_select_with_custom_id.bindValue(QSL(":custom_id"), unnulifyString(message.m_customId));

        qDebugNN << LOGSEC_DB << "Checking if message with service-specific custom ID"
                 << QUOTE_W_SPACE(message.m_customId) << "is present in DB.";

        if (query_select_with_custom_id.exec() && query_select_with_custom_id.next()) {
          id_existing_message = query_select_with_custom_id.value(0).toInt();
          date_existing_message = query_select_with_custom_id.value(1).value<qint64>();
          is_read_existing_message = query_select_with_custom_id.value(2).toBool();
          is_important_existing_message = query_select_with_custom_id.value(3).toBool();
          contents_existing_message = query_select_with_custom_id.value(4).toString();
          feed_id_existing_message = query_select_with_custom_id.value(5).toString();
          title_existing_message = query_select_with_custom_id.value(6).toString();
          author_existing_message = query_select_with_custom_id.value(7).toString();

          qDebugNN << LOGSEC_DB << "Message with custom ID" << QUOTE_W_SPACE(message.m_customId)
                   << "is already present in DB and has DB ID '" << id_existing_message << "'.";
        }
        else if (query_select_with_custom_id.lastError().isValid()) {
          qWarningNN << LOGSEC_DB << "Failed to check for existing message in DB via ID:"
                     << QUOTE_W_SPACE_DOT(query_select_with_custom_id.lastError().text());
        }

        query_select_with_custom_id.finish();
      }
      else {
        // Custom IDs are feed-specific.
        // NOTE: This concerns articles with ID/GUID from standard RSS/ATOM/JSON feeds.
        query_select_with_custom_id_for_feed.bindValue(QSL(":account_id"), account_id);
        query_select_with_custom_id_for_feed.bindValue(QSL(":feed"), feed_custom_id);
        query_select_with_custom_id_for_feed.bindValue(QSL(":custom_id"), unnulifyString(message.m_customId));

        qDebugNN << LOGSEC_DB << "Checking if message with feed-specific custom ID" << QUOTE_W_SPACE(message.m_customId)
                 << "is present in DB.";

        if (query_select_with_custom_id_for_feed.exec() && query_select_with_custom_id_for_feed.next()) {
          id_existing_message = query_select_with_custom_id_for_feed.value(0).toInt();
          date_existing_message = query_select_with_custom_id_for_feed.value(1).value<qint64>();
          is_read_existing_message = query_select_with_custom_id_for_feed.value(2).toBool();
          is_important_existing_message = query_select_with_custom_id_for_feed.value(3).toBool();
          contents_existing_message = query_select_with_custom_id_for_feed.value(4).toString();
          feed_id_existing_message = feed_custom_id;
          title_existing_message = query_select_with_custom_id_for_feed.value(5).toString();
          author_existing_message = query_select_with_custom_id_for_feed.value(6).toString();

          qDebugNN << LOGSEC_DB << "Message with custom ID" << QUOTE_W_SPACE(message.m_customId)
                   << "is already present in DB and has DB ID" << QUOTE_W_SPACE_DOT(id_existing_message);
        }
        else if (query_select_with_custom_id_for_feed.lastError().isValid()) {
          qWarningNN << LOGSEC_DB << "Failed to check for existing message in DB via ID:"
                     << QUOTE_W_SPACE_DOT(query_select_with_custom_id_for_feed.lastError().text());
        }

        query_select_with_custom_id_for_feed.finish();
      }
    }

    // Now, check if this message is already in the DB.
    if (id_existing_message >= 0) {
      message.m_id = id_existing_message;

      // Message is already in the DB.
      //
      // Now, we update it if at least one of next conditions is true:
      //   1) FOR SYNCHRONIZED SERVICES:
      //        Message has custom ID AND (its date OR read status OR starred status are changed
      //        or message was moved from other feed to current feed - this can particularly happen in Gmail feeds).
      //
      //   2) FOR NON-SYNCHRONIZED SERVICES (RSS/ATOM/JSON):
      //        Message has custom ID/GUID and its title or author or contents are changed.
      //
      //   3) FOR ALL SERVICES:
      //        Message has its date fetched from feed AND its date is different
      //        from date in DB or content is changed. Date/time is considered different
      //        when the difference is larger than MSG_DATETIME_DIFF_THRESSHOLD
      //
      //   4) FOR ALL SERVICES:
      //        Message update is forced, we want to overwrite message as some arbitrary atribute was changed,
      //        this particularly happens when manual message filter execution happens.
      bool ignore_contents_changes =
        qApp->settings()->value(GROUP(Messages), SETTING(Messages::IgnoreContentsChanges)).toBool();
      bool cond_1 =
        !message.m_customId.isEmpty() && feed->getParentServiceRoot()->isSyncable() &&
        (message.m_created.toMSecsSinceEpoch() != date_existing_message ||
         message.m_isRead != is_read_existing_message || message.m_isImportant != is_important_existing_message ||
         (message.m_feedId != feed_id_existing_message && message.m_feedId == feed_custom_id) ||
         message.m_title != title_existing_message ||
         (!ignore_contents_changes && message.m_contents != contents_existing_message));
      bool cond_2 = !message.m_customId.isEmpty() && !feed->getParentServiceRoot()->isSyncable() &&
                    (message.m_title != title_existing_message || message.m_author != author_existing_message ||
                     (!ignore_contents_changes && message.m_contents != contents_existing_message));
      bool cond_3 = (message.m_createdFromFeed && std::abs(message.m_created.toMSecsSinceEpoch() -
                                                           date_existing_message) > MSG_DATETIME_DIFF_THRESSHOLD) ||
                    (!ignore_contents_changes && message.m_contents != contents_existing_message);

      if (cond_1 || cond_2 || cond_3 || force_update) {
        if (!feed->getParentServiceRoot()->isSyncable()) {
          // Feed is not syncable, thus we got RSS/JSON/whatever.
          // Article is only updated, so we now prefer to keep original read state
          // pretty much the same way starred state is kept.
          message.m_isRead = is_read_existing_message;
        }

        // Message exists and is changed, update it.
        query_update.bindValue(QSL(":title"), unnulifyString(message.m_title));
        query_update.bindValue(QSL(":is_read"), int(message.m_isRead));
        query_update.bindValue(QSL(":is_important"),
                               (feed->getParentServiceRoot()->isSyncable() || message.m_isImportant)
                                 ? int(message.m_isImportant)
                                 : is_important_existing_message);
        query_update.bindValue(QSL(":is_deleted"), int(message.m_isDeleted));
        query_update.bindValue(QSL(":url"), unnulifyString(message.m_url));
        query_update.bindValue(QSL(":author"), unnulifyString(message.m_author));
        query_update.bindValue(QSL(":date_created"), message.m_created.toMSecsSinceEpoch());
        query_update.bindValue(QSL(":contents"), unnulifyString(message.m_contents));
        query_update.bindValue(QSL(":enclosures"), Enclosures::encodeEnclosuresToString(message.m_enclosures));
        query_update.bindValue(QSL(":feed"), message.m_feedId);
        query_update.bindValue(QSL(":score"), message.m_score);
        query_update.bindValue(QSL(":id"), id_existing_message);

        if (query_update.exec()) {
          qDebugNN << LOGSEC_DB << "Overwriting message with title" << QUOTE_W_SPACE(message.m_title) << "URL"
                   << QUOTE_W_SPACE(message.m_url) << "in DB.";

          if (!message.m_isRead) {
            updated_messages.m_unread.append(message);
          }

          updated_messages.m_all.append(message);
          message.m_insertedUpdated = true;
        }
        else if (query_update.lastError().isValid()) {
          qCriticalNN << LOGSEC_DB
                      << "Failed to update message in DB:" << QUOTE_W_SPACE_DOT(query_update.lastError().text());
        }

        query_update.finish();
      }
    }
    else {
      msgs_to_insert.append(&message);
    }
  }

  if (!msgs_to_insert.isEmpty()) {
    QString bulk_insert = QSL("INSERT INTO Messages "
                              "(feed, title, is_read, is_important, is_deleted, url, author, score, date_created, "
                              "contents, enclosures, custom_id, custom_hash, account_id) "
                              "VALUES %1;");

    for (int i = 0; i < msgs_to_insert.size(); i += 1000) {
      QStringList vals;
      int batch_length = std::min(1000, int(msgs_to_insert.size()) - i);

      for (int l = i; l < (i + batch_length); l++) {
        Message* msg = msgs_to_insert[l];

        if (msg->m_title.isEmpty()) {
          qCriticalNN << LOGSEC_DB << "Message" << QUOTE_W_SPACE(msg->m_customId)
                      << "will not be inserted to DB because it does not meet DB constraints.";

          continue;
        }

        vals.append(QSL("\n(':feed', ':title', :is_read, :is_important, :is_deleted, "
                        "':url', ':author', :score, :date_created, ':contents', ':enclosures', "
                        "':custom_id', ':custom_hash', :account_id)")
                      .replace(QSL(":feed"), unnulifyString(feed_custom_id))
                      .replace(QSL(":title"), DatabaseFactory::escapeQuery(unnulifyString(msg->m_title)))
                      .replace(QSL(":is_read"), QString::number(int(msg->m_isRead)))
                      .replace(QSL(":is_important"), QString::number(int(msg->m_isImportant)))
                      .replace(QSL(":is_deleted"), QString::number(int(msg->m_isDeleted)))
                      .replace(QSL(":url"), DatabaseFactory::escapeQuery(unnulifyString(msg->m_url)))
                      .replace(QSL(":author"), DatabaseFactory::escapeQuery(unnulifyString(msg->m_author)))
                      .replace(QSL(":date_created"), QString::number(msg->m_created.toMSecsSinceEpoch()))
                      .replace(QSL(":contents"), DatabaseFactory::escapeQuery(unnulifyString(msg->m_contents)))
                      .replace(QSL(":enclosures"),
                               DatabaseFactory::escapeQuery(Enclosures::encodeEnclosuresToString(msg->m_enclosures)))
                      .replace(QSL(":custom_id"), DatabaseFactory::escapeQuery(unnulifyString(msg->m_customId)))
                      .replace(QSL(":custom_hash"), unnulifyString(msg->m_customHash))
                      .replace(QSL(":score"), QString::number(msg->m_score))
                      .replace(QSL(":account_id"), QString::number(account_id)));
      }

      if (!vals.isEmpty()) {
        QString final_bulk = bulk_insert.arg(vals.join(QSL(", ")));

        QMutexLocker lck(db_mutex);

        auto bulk_query = QSqlQuery(final_bulk, db);
        auto bulk_error = bulk_query.lastError();

        if (bulk_error.isValid()) {
          QString txt = bulk_error.text() + bulk_error.databaseText() + bulk_error.driverText();

          qCriticalNN << LOGSEC_DB << "Failed bulk insert of articles:" << QUOTE_W_SPACE_DOT(txt);
        }
        else {
          // OK, we bulk-inserted many messages but the thing is that they do not
          // have their DB IDs fetched in objects, therefore labels cannot be assigned etc.
          //
          // We can calculate real IDs because of how "auto-increment" algorithms work.
          //   https://www.sqlite.org/autoinc.html
          //   https://mariadb.com/kb/en/auto_increment
          int last_msg_id = bulk_query.lastInsertId().toInt();

          for (int l = i, c = 1; l < (i + batch_length); l++, c++) {
            Message* msg = msgs_to_insert[l];

            if (msg->m_title.isEmpty()) {
              // This article was not for sure inserted. Tweak
              // next ID calculation.
              c--;
              continue;
            }

            msg->m_insertedUpdated = true;
            msg->m_id = last_msg_id - batch_length + c;

            if (!msg->m_isRead) {
              updated_messages.m_unread.append(*msg);
            }

            updated_messages.m_all.append(*msg);
          }
        }
      }
    }
  }

  const bool uses_online_labels = Globals::hasFlag(feed->getParentServiceRoot()->supportedLabelOperations(),
                                                   ServiceRoot::LabelOperation::Synchronised);

  for (Message& message : messages) {
    if (!message.m_customId.isEmpty() || message.m_id > 0) {
      QMutexLocker lck(db_mutex);
      // bool lbls_changed = false;

      if (uses_online_labels) {
        // Store all labels obtained from server.
        setLabelsForMessage(db, message.m_assignedLabels, message);
        // lbls_changed = true;
      }

      // Adjust labels tweaked by filters.
      for (Label* assigned_by_filter : message.m_assignedLabelsByFilter) {
        assigned_by_filter->assignToMessage(message, false);
        // lbls_changed = true;
      }

      for (Label* removed_by_filter : message.m_deassignedLabelsByFilter) {
        removed_by_filter->deassignFromMessage(message, false);
        // lbls_changed = true;
      }

      // NOTE: This is likely not needed at all
      // as this feature was semi-broken anyway, it was trigerred
      // even when labels were added (via article filtering or on service server)
      // and the same labels were already assigned in local DB.
      // In other words, label assignment differences were not correctly
      // taken into account.
      /*
      if (lbls_changed && !message.m_insertedUpdated) {
        // This article was not inserted/updated in DB because its contents did not change
        // but its assigned labels were changed. Therefore we must count article
        // as updated.
        if (!message.m_isRead) {
          updated_messages.m_unread.append(message);
        }

        updated_messages.m_all.append(message);
      }
      */
    }
    else {
      qCriticalNN << LOGSEC_DB << "Cannot set labels for message" << QUOTE_W_SPACE(message.m_title)
                  << "because we don't have ID or custom ID.";
    }
  }

  // Now, fixup custom IDS for messages which initially did not have them,
  // just to keep the data consistent.
  QMutexLocker lck(db_mutex);

  QSqlQuery fixup_custom_ids_query(QSL("UPDATE Messages "
                                       "SET custom_id = id "
                                       "WHERE custom_id IS NULL OR custom_id = '';"),
                                   db);
  QSqlError fixup_custom_ids_error = fixup_custom_ids_query.lastError();

  if (fixup_custom_ids_error.isValid()) {
    qCriticalNN << LOGSEC_DB
                << "Failed to set custom ID for all messages:" << QUOTE_W_SPACE_DOT(fixup_custom_ids_error.text());
  }

  if (ok != nullptr) {
    *ok = true;
  }

  return updated_messages;
}

bool DatabaseQueries::purgeMessagesFromBin(const QSqlDatabase& db, bool clear_only_read, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (clear_only_read) {
    q.prepare(QSL("UPDATE Messages SET is_pdeleted = 1 WHERE is_read = 1 AND is_deleted = 1 AND account_id = "
                  ":account_id;"));
  }
  else {
    q.prepare(QSL("UPDATE Messages SET is_pdeleted = 1 WHERE is_deleted = 1 AND account_id = :account_id;"));
  }

  q.bindValue(QSL(":account_id"), account_id);
  return q.exec();
}

bool DatabaseQueries::deleteAccount(const QSqlDatabase& db, ServiceRoot* account) {
  moveItem(account, false, true, {}, db);

  QSqlQuery query(db);

  query.setForwardOnly(true);
  QStringList queries;

  queries << QSL("DELETE FROM MessageFiltersInFeeds WHERE account_id = :account_id;")
          << QSL("DELETE FROM Messages WHERE account_id = :account_id;")
          << QSL("DELETE FROM Feeds WHERE account_id = :account_id;")
          << QSL("DELETE FROM Categories WHERE account_id = :account_id;")
          << QSL("DELETE FROM Labels WHERE account_id = :account_id;")
          << QSL("DELETE FROM Accounts WHERE id = :account_id;");

  for (const QString& q : std::as_const(queries)) {
    query.prepare(q);
    query.bindValue(QSL(":account_id"), account->accountId());

    if (!query.exec()) {
      qCriticalNN << LOGSEC_DB << "Removing of account from DB failed, this is critical: '" << query.lastError().text()
                  << "'.";
      return false;
    }
    else {
      query.finish();
    }
  }

  return true;
}

bool DatabaseQueries::deleteAccountData(const QSqlDatabase& db,
                                        int account_id,
                                        bool delete_messages_too,
                                        bool delete_labels_too) {
  bool result = true;
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (delete_messages_too) {
    q.prepare(QSL("DELETE FROM Messages WHERE account_id = :account_id;"));
    q.bindValue(QSL(":account_id"), account_id);
    result &= q.exec();
  }

  q.prepare(QSL("DELETE FROM Feeds WHERE account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);
  result &= q.exec();

  q.prepare(QSL("DELETE FROM Categories WHERE account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);
  result &= q.exec();

  if (delete_labels_too) {
    q.prepare(QSL("DELETE FROM Labels WHERE account_id = :account_id;"));
    q.bindValue(QSL(":account_id"), account_id);
    result &= q.exec();
  }

  return result;
}

bool DatabaseQueries::cleanLabelledMessages(const QSqlDatabase& db, bool clean_read_only, Label* label) {
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (clean_read_only) {
    q.prepare(QSL("UPDATE Messages SET is_deleted = :deleted "
                  "WHERE "
                  "  is_deleted = 0 AND "
                  "  is_pdeleted = 0 AND "
                  "  is_read = 1 AND "
                  "  account_id = :account_id AND "
                  "  labels LIKE :label;"));
  }
  else {
    q.prepare(QSL("UPDATE Messages SET is_deleted = :deleted "
                  "WHERE "
                  "  is_deleted = 0 AND "
                  "  is_pdeleted = 0 AND "
                  "  account_id = :account_id AND "
                  "  labels LIKE :label;"));
  }

  q.bindValue(QSL(":deleted"), 1);
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":label"), QSL("%.%1.%").arg(label->customId()));

  if (!q.exec()) {
    qWarningNN << LOGSEC_DB << "Cleaning of labelled messages failed:" << QUOTE_W_SPACE_DOT(q.lastError().text());
    return false;
  }
  else {
    return true;
  }
}

void DatabaseQueries::cleanProbedMessages(const QSqlDatabase& db, bool clean_read_only, Search* probe) {
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (clean_read_only) {
    q.prepare(QSL("UPDATE Messages SET is_deleted = :deleted "
                  "WHERE "
                  "  is_deleted = 0 AND "
                  "  is_pdeleted = 0 AND "
                  "  is_read = 1 AND "
                  "  account_id = :account_id AND "
                  "  (title REGEXP :fltr OR contents REGEXP :fltr);"));
  }
  else {
    q.prepare(QSL("UPDATE Messages SET is_deleted = :deleted "
                  "WHERE "
                  "  is_deleted = 0 AND "
                  "  is_pdeleted = 0 AND "
                  "  account_id = :account_id AND "
                  "  (title REGEXP :fltr OR contents REGEXP :fltr);"));
  }

  q.bindValue(QSL(":deleted"), 1);
  q.bindValue(QSL(":account_id"), probe->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":fltr"), probe->filter());

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
}

bool DatabaseQueries::cleanImportantMessages(const QSqlDatabase& db, bool clean_read_only, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (clean_read_only) {
    q.prepare(QSL("UPDATE Messages SET is_deleted = :deleted "
                  "WHERE is_important = 1 AND is_deleted = 0 AND is_pdeleted = 0 AND is_read = 1 AND account_id = "
                  ":account_id;"));
  }
  else {
    q.prepare(QSL("UPDATE Messages SET is_deleted = :deleted "
                  "WHERE is_important = 1 AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;"));
  }

  q.bindValue(QSL(":deleted"), 1);
  q.bindValue(QSL(":account_id"), account_id);

  if (!q.exec()) {
    qWarningNN << LOGSEC_DB << "Cleaning of important messages failed: '" << q.lastError().text() << "'.";
    return false;
  }
  else {
    return true;
  }
}

bool DatabaseQueries::cleanUnreadMessages(const QSqlDatabase& db, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_deleted = :deleted "
                "WHERE is_deleted = 0 AND is_pdeleted = 0 AND is_read = 0 AND account_id = :account_id;"));

  q.bindValue(QSL(":deleted"), 1);
  q.bindValue(QSL(":account_id"), account_id);

  if (!q.exec()) {
    qWarningNN << LOGSEC_DB << "Cleaning of unread messages failed: '" << q.lastError().text() << "'.";
    return false;
  }
  else {
    return true;
  }
}

bool DatabaseQueries::cleanFeeds(const QSqlDatabase& db, const QStringList& ids, bool clean_read_only, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);

  if (clean_read_only) {
    q.prepare(QString("UPDATE Messages SET is_deleted = :deleted "
                      "WHERE feed IN (%1) AND is_deleted = 0 AND is_pdeleted = 0 AND is_read = 1 AND account_id = "
                      ":account_id;")
                .arg(ids.join(QSL(", "))));
  }
  else {
    q.prepare(QString("UPDATE Messages SET is_deleted = :deleted "
                      "WHERE feed IN (%1) AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;")
                .arg(ids.join(QSL(", "))));
  }

  q.bindValue(QSL(":deleted"), 1);
  q.bindValue(QSL(":account_id"), account_id);

  if (!q.exec()) {
    qWarningNN << LOGSEC_DB << "Cleaning of feeds failed: '" << q.lastError().text() << "'.";
    return false;
  }
  else {
    return true;
  }
}

bool DatabaseQueries::purgeLeftoverMessageFilterAssignments(const QSqlDatabase& db, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM MessageFiltersInFeeds "
                "WHERE account_id = :account_id AND "
                "feed_custom_id NOT IN (SELECT custom_id FROM Feeds WHERE account_id = :account_id);"));
  q.bindValue(QSL(":account_id"), account_id);

  if (!q.exec()) {
    qWarningNN << LOGSEC_DB << "Removing of leftover message filter assignments failed: '" << q.lastError().text()
               << "'.";
    return false;
  }
  else {
    return true;
  }
}

bool DatabaseQueries::purgeLeftoverMessages(const QSqlDatabase& db, int account_id) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Messages "
                "WHERE account_id = :account_id AND feed NOT IN (SELECT custom_id FROM Feeds WHERE account_id = "
                ":account_id);"));
  q.bindValue(QSL(":account_id"), account_id);

  if (!q.exec()) {
    qWarningNN << LOGSEC_DB << "Removing of leftover messages failed: '" << q.lastError().text() << "'.";
    return false;
  }
  else {
    return true;
  }
}

void DatabaseQueries::storeAccountTree(const QSqlDatabase& db, RootItem* tree_root, int account_id) {
  // Iterate all children.
  auto str = tree_root->getSubTree<RootItem>();

  for (RootItem* child : std::as_const(str)) {
    if (child->kind() == RootItem::Kind::Category) {
      createOverwriteCategory(db, child->toCategory(), account_id, child->parent()->id());
    }
    else if (child->kind() == RootItem::Kind::Feed) {
      createOverwriteFeed(db, child->toFeed(), account_id, child->parent()->id());
    }
    else if (child->kind() == RootItem::Kind::Labels) {
      // Add all labels.
      auto ch = child->childItems();

      for (RootItem* lbl : std::as_const(ch)) {
        Label* label = lbl->toLabel();

        createLabel(db, label, account_id);
      }
    }
  }
}

QStringList DatabaseQueries::customIdsOfMessagesFromAccount(const QSqlDatabase& db,
                                                            RootItem::ReadStatus target_read,
                                                            int account_id,
                                                            bool* ok) {
  QSqlQuery q(db);
  QStringList ids;

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT custom_id FROM Messages "
                "WHERE is_read = :read AND is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":read"), target_read == RootItem::ReadStatus::Read ? 0 : 1);

  if (ok != nullptr) {
    *ok = q.exec();
  }
  else {
    q.exec();
  }

  while (q.next()) {
    ids.append(q.value(0).toString());
  }

  return ids;
}

QStringList DatabaseQueries::customIdsOfMessagesFromLabel(const QSqlDatabase& db,
                                                          Label* label,
                                                          RootItem::ReadStatus target_read,
                                                          bool* ok) {
  QSqlQuery q(db);
  QStringList ids;

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT custom_id FROM Messages "
                "WHERE "
                "    is_read = :read AND "
                "    is_deleted = 0 AND "
                "    is_pdeleted = 0 AND "
                "    account_id = :account_id AND "
                "    labels LIKE :label;"));
  q.bindValue(QSL(":account_id"), label->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":label"), QSL("%.%1.%").arg(label->customId()));
  q.bindValue(QSL(":read"), target_read == RootItem::ReadStatus::Read ? 0 : 1);

  if (ok != nullptr) {
    *ok = q.exec();
  }
  else {
    q.exec();
  }

  while (q.next()) {
    ids.append(q.value(0).toString());
  }

  return ids;
}

void DatabaseQueries::markProbeReadUnread(const QSqlDatabase& db, Search* probe, RootItem::ReadStatus read) {
  QSqlQuery q(db);

  q.setForwardOnly(true);
  q.prepare(QSL("UPDATE Messages SET is_read = :read "
                "WHERE "
                "    is_deleted = 0 AND "
                "    is_pdeleted = 0 AND "
                "    account_id = :account_id AND "
                "    (title REGEXP :fltr OR contents REGEXP :fltr);"));
  q.bindValue(QSL(":read"), read == RootItem::ReadStatus::Read ? 1 : 0);
  q.bindValue(QSL(":account_id"), probe->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":fltr"), probe->filter());

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
}

QStringList DatabaseQueries::customIdsOfMessagesFromProbe(const QSqlDatabase& db,
                                                          Search* probe,
                                                          RootItem::ReadStatus target_read) {
  QSqlQuery q(db);
  QStringList ids;

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT custom_id FROM Messages "
                "WHERE "
                "    is_read = :read AND "
                "    is_deleted = 0 AND "
                "    is_pdeleted = 0 AND "
                "    account_id = :account_id AND "
                "    (title REGEXP :fltr OR contents REGEXP :fltr);"));
  q.bindValue(QSL(":account_id"), probe->getParentServiceRoot()->accountId());
  q.bindValue(QSL(":read"), target_read == RootItem::ReadStatus::Read ? 0 : 1);
  q.bindValue(QSL(":fltr"), probe->filter());

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }

  while (q.next()) {
    ids.append(q.value(0).toString());
  }

  return ids;
}

QStringList DatabaseQueries::customIdsOfImportantMessages(const QSqlDatabase& db,
                                                          RootItem::ReadStatus target_read,
                                                          int account_id,
                                                          bool* ok) {
  QSqlQuery q(db);
  QStringList ids;

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT custom_id FROM Messages "
                "WHERE "
                "is_read = :read AND is_important = 1 AND is_deleted = 0 AND "
                "is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":read"), target_read == RootItem::ReadStatus::Read ? 0 : 1);

  if (ok != nullptr) {
    *ok = q.exec();
  }
  else {
    q.exec();
  }

  while (q.next()) {
    ids.append(q.value(0).toString());
  }

  return ids;
}

QStringList DatabaseQueries::customIdsOfUnreadMessages(const QSqlDatabase& db, int account_id, bool* ok) {
  QSqlQuery q(db);
  QStringList ids;

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT custom_id FROM Messages "
                "WHERE is_read = 0 AND is_deleted = 0 AND is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);

  if (ok != nullptr) {
    *ok = q.exec();
  }
  else {
    q.exec();
  }

  while (q.next()) {
    ids.append(q.value(0).toString());
  }

  return ids;
}

QStringList DatabaseQueries::customIdsOfMessagesFromBin(const QSqlDatabase& db,
                                                        RootItem::ReadStatus target_read,
                                                        int account_id,
                                                        bool* ok) {
  QSqlQuery q(db);
  QStringList ids;

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT custom_id FROM Messages "
                "WHERE is_read = :read AND is_deleted = 1 AND is_pdeleted = 0 AND account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":read"), target_read == RootItem::ReadStatus::Read ? 0 : 1);

  if (ok != nullptr) {
    *ok = q.exec();
  }
  else {
    q.exec();
  }

  while (q.next()) {
    ids.append(q.value(0).toString());
  }

  return ids;
}

QStringList DatabaseQueries::customIdsOfMessagesFromFeed(const QSqlDatabase& db,
                                                         const QString& feed_custom_id,
                                                         RootItem::ReadStatus target_read,
                                                         int account_id,
                                                         bool* ok) {
  QSqlQuery q(db);
  QStringList ids;

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT custom_id FROM Messages "
                "WHERE is_read = :read AND is_deleted = 0 AND "
                "is_pdeleted = 0 AND feed = :feed AND account_id = :account_id;"));
  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":feed"), feed_custom_id);
  q.bindValue(QSL(":read"), target_read == RootItem::ReadStatus::Read ? 0 : 1);

  if (ok != nullptr) {
    *ok = q.exec();
  }
  else {
    q.exec();
  }

  while (q.next()) {
    ids.append(q.value(0).toString());
  }

  return ids;
}

void DatabaseQueries::createOverwriteCategory(const QSqlDatabase& db,
                                              Category* category,
                                              int account_id,
                                              int new_parent_id) {
  QSqlQuery q(db);
  int next_sort_order;

  if (category->id() <= 0 || (category->parent() != nullptr && category->parent()->id() != new_parent_id)) {
    q.prepare(QSL("SELECT MAX(ordr) FROM Categories WHERE account_id = :account_id AND parent_id = :parent_id;"));
    q.bindValue(QSL(":account_id"), account_id);
    q.bindValue(QSL(":parent_id"), new_parent_id);

    if (!q.exec() || !q.next()) {
      throw ApplicationException(q.lastError().text());
    }

    next_sort_order = (q.value(0).isNull() ? -1 : q.value(0).toInt()) + 1;
    q.finish();
  }
  else {
    next_sort_order = category->sortOrder();
  }

  if (category->id() <= 0) {
    // We need to insert category first.
    q.prepare(QSL("INSERT INTO "
                  "Categories (parent_id, ordr, title, date_created, account_id) "
                  "VALUES (0, 0, 'new', 0, %1);")
                .arg(QString::number(account_id)));

    if (!q.exec()) {
      throw ApplicationException(q.lastError().text());
    }
    else {
      category->setId(q.lastInsertId().toInt());
    }
  }
  else if (category->parent() != nullptr && category->parent()->id() != new_parent_id) {
    // Category is moving between parents.
    // 1. Move category to bottom of current parent.
    // 2. Assign proper new sort order.
    //
    // NOTE: The category will get reassigned to new parent usually after this method
    // completes by the caller.
    moveItem(category, false, true, {}, db);
  }

  // Restore to correct sort order.
  category->setSortOrder(next_sort_order);

  q.prepare("UPDATE Categories "
            "SET parent_id = :parent_id, ordr = :ordr, title = :title, description = :description, date_created = "
            ":date_created, "
            "    icon = :icon, account_id = :account_id, custom_id = :custom_id "
            "WHERE id = :id;");
  q.bindValue(QSL(":parent_id"), new_parent_id);
  q.bindValue(QSL(":title"), category->title());
  q.bindValue(QSL(":description"), category->description());
  q.bindValue(QSL(":date_created"), category->creationDate().toMSecsSinceEpoch());
  q.bindValue(QSL(":icon"), qApp->icons()->toByteArray(category->icon()));
  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":custom_id"), category->customId());
  q.bindValue(QSL(":id"), category->id());
  q.bindValue(QSL(":ordr"), category->sortOrder());

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
}

void DatabaseQueries::createOverwriteFeed(const QSqlDatabase& db, Feed* feed, int account_id, int new_parent_id) {
  QSqlQuery q(db);
  int next_sort_order;

  if (feed->id() <= 0 || (feed->parent() != nullptr && feed->parent()->id() != new_parent_id)) {
    // We either insert completely new feed or we move feed
    // to new parent. Get new viable sort order.
    q.prepare(QSL("SELECT MAX(ordr) FROM Feeds WHERE account_id = :account_id AND category = :category;"));
    q.bindValue(QSL(":account_id"), account_id);
    q.bindValue(QSL(":category"), new_parent_id);

    if (!q.exec() || !q.next()) {
      throw ApplicationException(q.lastError().text());
    }

    next_sort_order = (q.value(0).isNull() ? -1 : q.value(0).toInt()) + 1;
    q.finish();
  }
  else {
    next_sort_order = feed->sortOrder();
  }

  if (feed->id() <= 0) {
    // We need to insert feed first.
    q.prepare(QSL("INSERT INTO "
                  "Feeds (title, ordr, date_created, category, update_type, update_interval, account_id, custom_id) "
                  "VALUES ('new', 0, 0, 0, 0, 1, %1, 'new');")
                .arg(QString::number(account_id)));

    if (!q.exec()) {
      throw ApplicationException(q.lastError().text());
    }
    else {
      feed->setId(q.lastInsertId().toInt());

      if (feed->customId().isEmpty()) {
        feed->setCustomId(QString::number(feed->id()));
      }
    }
  }
  else if (feed->parent() != nullptr && feed->parent()->id() != new_parent_id) {
    // Feed is moving between categories.
    // 1. Move feed to bottom of current category.
    // 2. Assign proper new sort order.
    //
    // NOTE: The feed will get reassigned to new parent usually after this method
    // completes by the caller.
    moveItem(feed, false, true, {}, db);
  }

  // Restore to correct sort order.
  feed->setSortOrder(next_sort_order);

  q.prepare("UPDATE Feeds "
            "SET "
            "title = :title, "
            "ordr = :ordr, "
            "description = :description, "
            "date_created = :date_created, "
            "icon = :icon, "
            "category = :category, "
            "source = :source, "
            "update_type = :update_type, "
            "update_interval = :update_interval, "
            "is_off = :is_off, "
            "is_quiet = :is_quiet, "
            "is_rtl = :is_rtl, "
            "add_any_datetime_articles = :add_any_datetime_articles, "
            "datetime_to_avoid = :datetime_to_avoid, "
            "keep_article_customize = :keep_article_customize, "
            "keep_article_count = :keep_article_count, "
            "keep_unread_articles = :keep_unread_articles, "
            "keep_starred_articles = :keep_starred_articles, "
            "recycle_articles = :recycle_articles, "
            "account_id = :account_id, "
            "custom_id = :custom_id, "
            "custom_data = :custom_data "
            "WHERE id = :id;");
  q.bindValue(QSL(":title"), feed->title());
  q.bindValue(QSL(":description"), feed->description());
  q.bindValue(QSL(":date_created"), feed->creationDate().toMSecsSinceEpoch());
  q.bindValue(QSL(":icon"), qApp->icons()->toByteArray(feed->icon()));
  q.bindValue(QSL(":category"), new_parent_id);
  q.bindValue(QSL(":source"), feed->source());
  q.bindValue(QSL(":update_type"), int(feed->autoUpdateType()));
  q.bindValue(QSL(":update_interval"), feed->autoUpdateInterval());
  q.bindValue(QSL(":account_id"), account_id);
  q.bindValue(QSL(":custom_id"), feed->customId());
  q.bindValue(QSL(":id"), feed->id());
  q.bindValue(QSL(":ordr"), feed->sortOrder());
  q.bindValue(QSL(":is_off"), feed->isSwitchedOff());
  q.bindValue(QSL(":is_quiet"), feed->isQuiet());
  q.bindValue(QSL(":is_rtl"), int(feed->rtlBehavior()));

  const Feed::ArticleIgnoreLimit art = feed->articleIgnoreLimit();

  q.bindValue(QSL(":add_any_datetime_articles"), art.m_addAnyArticlesToDb);
  q.bindValue(QSL(":datetime_to_avoid"),
              (art.m_dtToAvoid.isValid() && art.m_dtToAvoid.toMSecsSinceEpoch() > 0)
                ? art.m_dtToAvoid.toMSecsSinceEpoch()
                : art.m_hoursToAvoid);

  q.bindValue(QSL(":keep_article_customize"), art.m_customizeLimitting);
  q.bindValue(QSL(":keep_article_count"), art.m_keepCountOfArticles);
  q.bindValue(QSL(":keep_unread_articles"), art.m_doNotRemoveUnread);
  q.bindValue(QSL(":keep_starred_articles"), art.m_doNotRemoveStarred);
  q.bindValue(QSL(":recycle_articles"), art.m_moveToBinDontPurge);

  auto custom_data = feed->customDatabaseData();
  QString serialized_custom_data = serializeCustomData(custom_data);

  q.bindValue(QSL(":custom_data"), serialized_custom_data);

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
}

void DatabaseQueries::createOverwriteAccount(const QSqlDatabase& db, ServiceRoot* account) {
  QSqlQuery q(db);

  if (account->accountId() <= 0) {
    // We need to insert account and generate sort order first.
    if (account->sortOrder() < 0) {
      if (!q.exec(QSL("SELECT MAX(ordr) FROM Accounts;"))) {
        throw ApplicationException(q.lastError().text());
      }

      q.next();

      int next_order = (q.value(0).isNull() ? -1 : q.value(0).toInt()) + 1;

      account->setSortOrder(next_order);
      q.finish();
    }

    q.prepare(QSL("INSERT INTO Accounts (ordr, type) "
                  "VALUES (0, :type);"));
    q.bindValue(QSL(":type"), account->code());

    if (!q.exec()) {
      throw ApplicationException(q.lastError().text());
    }
    else {
      account->setAccountId(q.lastInsertId().toInt());
    }
  }

  // Now we construct the SQL update query.
  auto proxy = account->networkProxy();

  q.prepare(QSL("UPDATE Accounts "
                "SET proxy_type = :proxy_type, proxy_host = :proxy_host, proxy_port = :proxy_port, "
                "    proxy_username = :proxy_username, proxy_password = :proxy_password, ordr = :ordr, "
                "    custom_data = :custom_data "
                "WHERE id = :id"));
  q.bindValue(QSL(":proxy_type"), proxy.type());
  q.bindValue(QSL(":proxy_host"), proxy.hostName());
  q.bindValue(QSL(":proxy_port"), proxy.port());
  q.bindValue(QSL(":proxy_username"), proxy.user());
  q.bindValue(QSL(":proxy_password"), TextFactory::encrypt(proxy.password()));
  q.bindValue(QSL(":id"), account->accountId());
  q.bindValue(QSL(":ordr"), account->sortOrder());

  auto custom_data = account->customDatabaseData();
  QString serialized_custom_data = serializeCustomData(custom_data);

  q.bindValue(QSL(":custom_data"), serialized_custom_data);

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }
}

bool DatabaseQueries::deleteFeed(const QSqlDatabase& db, Feed* feed, int account_id) {
  moveItem(feed, false, true, {}, db);

  QSqlQuery q(db);

  q.prepare(QSL("DELETE FROM Messages WHERE feed = :feed AND account_id = :account_id;"));
  q.bindValue(QSL(":feed"), feed->customId());
  q.bindValue(QSL(":account_id"), account_id);

  if (!q.exec()) {
    return false;
  }

  // Remove feed itself.
  q.prepare(QSL("DELETE FROM Feeds WHERE custom_id = :feed AND account_id = :account_id;"));
  q.bindValue(QSL(":feed"), feed->customId());
  q.bindValue(QSL(":account_id"), account_id);

  return q.exec() && purgeLeftoverMessageFilterAssignments(db, account_id);
}

bool DatabaseQueries::deleteCategory(const QSqlDatabase& db, Category* category) {
  moveItem(category, false, true, {}, db);

  QSqlQuery q(db);

  // Remove this category from database.
  q.setForwardOnly(true);
  q.prepare(QSL("DELETE FROM Categories WHERE id = :category;"));
  q.bindValue(QSL(":category"), category->id());
  return q.exec();
}

void DatabaseQueries::moveItem(RootItem* item,
                               bool move_top,
                               bool move_bottom,
                               int move_index,
                               const QSqlDatabase& db) {
  if (item->kind() != RootItem::Kind::Feed && item->kind() != RootItem::Kind::Category &&
      item->kind() != RootItem::Kind::ServiceRoot) {
    return;
  }

  auto neighbors = item->parent()->childItems();
  int max_sort_order = boolinq::from(neighbors)
                         .select([=](RootItem* it) {
                           return it->kind() == item->kind() ? it->sortOrder() : 0;
                         })
                         .max();

  if ((!move_top && !move_bottom && item->sortOrder() == move_index) || /* Item is already sorted OK. */
      (!move_top && !move_bottom &&
       move_index < 0) || /* Order cannot be smaller than 0 if we do not move to begin/end. */
      (!move_top && !move_bottom && move_index > max_sort_order) || /* Cannot move past biggest sort order. */
      (move_top && item->sortOrder() == 0) ||                       /* Item is already on top. */
      (move_bottom && item->sortOrder() == max_sort_order) ||       /* Item is already on bottom. */
      max_sort_order <= 0) {                                        /* We only have 1 item, nothing to sort. */
    return;
  }

  QSqlQuery q(db);

  if (move_top) {
    move_index = 0;
  }
  else if (move_bottom) {
    move_index = max_sort_order;
  }

  int move_low = qMin(move_index, item->sortOrder());
  int move_high = qMax(move_index, item->sortOrder());
  QString parent_field, table_name;

  switch (item->kind()) {
    case RootItem::Kind::Feed:
      parent_field = QSL("category");
      table_name = QSL("Feeds");
      break;

    case RootItem::Kind::Category:
      parent_field = QSL("parent_id");
      table_name = QSL("Categories");
      break;

    case RootItem::Kind::ServiceRoot:
      table_name = QSL("Accounts");
      break;
  }

  if (item->kind() == RootItem::Kind::ServiceRoot) {
    if (item->sortOrder() > move_index) {
      q.prepare(QSL("UPDATE Accounts SET ordr = ordr + 1 "
                    "WHERE ordr < :move_high AND ordr >= :move_low;"));
    }
    else {
      q.prepare(QSL("UPDATE Accounts SET ordr = ordr - 1 "
                    "WHERE ordr > :move_low AND ordr <= :move_high;"));
    }
  }
  else {
    if (item->sortOrder() > move_index) {
      q.prepare(QSL("UPDATE %1 SET ordr = ordr + 1 "
                    "WHERE account_id = :account_id AND %2 = :category AND ordr < :move_high AND ordr >= :move_low;")
                  .arg(table_name, parent_field));
    }
    else {
      q.prepare(QSL("UPDATE %1 SET ordr = ordr - 1 "
                    "WHERE account_id = :account_id AND %2 = :category AND ordr > :move_low AND ordr <= :move_high;")
                  .arg(table_name, parent_field));
    }

    q.bindValue(QSL(":account_id"), item->getParentServiceRoot()->accountId());
    q.bindValue(QSL(":category"), item->parent()->id());
  }

  q.bindValue(QSL(":move_low"), move_low);
  q.bindValue(QSL(":move_high"), move_high);

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }

  q.prepare(QSL("UPDATE %1 SET ordr = :ordr WHERE id = :id;").arg(table_name));
  q.bindValue(QSL(":id"),
              item->kind() == RootItem::Kind::ServiceRoot ? item->toServiceRoot()->accountId() : item->id());
  q.bindValue(QSL(":ordr"), move_index);

  if (!q.exec()) {
    throw ApplicationException(q.lastError().text());
  }

  // Fix live sort orders.
  if (item->sortOrder() > move_index) {
    boolinq::from(neighbors)
      .where([=](RootItem* it) {
        return it->kind() == item->kind() && it->sortOrder() < move_high && it->sortOrder() >= move_low;
      })
      .for_each([](RootItem* it) {
        it->setSortOrder(it->sortOrder() + 1);
      });
  }
  else {
    boolinq::from(neighbors)
      .where([=](RootItem* it) {
        return it->kind() == item->kind() && it->sortOrder() > move_low && it->sortOrder() <= move_high;
      })
      .for_each([](RootItem* it) {
        it->setSortOrder(it->sortOrder() - 1);
      });
  }

  item->setSortOrder(move_index);
}

MessageFilter* DatabaseQueries::addMessageFilter(const QSqlDatabase& db, const QString& title, const QString& script) {
  if (!db.driver()->hasFeature(QSqlDriver::DriverFeature::LastInsertId)) {
    throw ApplicationException(QObject::tr("Cannot insert article filter, because current database cannot return last "
                                           "inserted row ID."));
  }

  QSqlQuery q(db);

  q.prepare(QSL("INSERT INTO MessageFilters (name, script) VALUES(:name, :script);"));

  q.bindValue(QSL(":name"), title);
  q.bindValue(QSL(":script"), script);
  q.setForwardOnly(true);

  if (q.exec()) {
    auto* fltr = new MessageFilter(q.lastInsertId().toInt());

    fltr->setName(title);
    fltr->setScript(script);

    return fltr;
  }
  else {
    throw ApplicationException(q.lastError().text());
  }
}

void DatabaseQueries::removeMessageFilter(const QSqlDatabase& db, int filter_id, bool* ok) {
  QSqlQuery q(db);

  q.prepare(QSL("DELETE FROM MessageFilters WHERE id = :id;"));

  q.bindValue(QSL(":id"), filter_id);
  q.setForwardOnly(true);

  if (q.exec()) {
    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }
}

void DatabaseQueries::removeMessageFilterAssignments(const QSqlDatabase& db, int filter_id, bool* ok) {
  QSqlQuery q(db);

  q.prepare(QSL("DELETE FROM MessageFiltersInFeeds WHERE filter = :filter;"));

  q.bindValue(QSL(":filter"), filter_id);
  q.setForwardOnly(true);

  if (q.exec()) {
    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }
}

QList<MessageFilter*> DatabaseQueries::getMessageFilters(const QSqlDatabase& db, bool* ok) {
  QSqlQuery q(db);
  QList<MessageFilter*> filters;

  q.setForwardOnly(true);
  q.prepare(QSL("SELECT id, name, script FROM MessageFilters;"));

  if (q.exec()) {
    while (q.next()) {
      auto* filter = new MessageFilter(q.value(0).toInt());

      filter->setName(q.value(1).toString());
      filter->setScript(q.value(2).toString());

      filters.append(filter);
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return filters;
}

QMultiMap<QString, int> DatabaseQueries::messageFiltersInFeeds(const QSqlDatabase& db, int account_id, bool* ok) {
  QSqlQuery q(db);
  QMultiMap<QString, int> filters_in_feeds;

  q.prepare(QSL("SELECT filter, feed_custom_id FROM MessageFiltersInFeeds WHERE account_id = :account_id;"));

  q.bindValue(QSL(":account_id"), account_id);
  q.setForwardOnly(true);

  if (q.exec()) {
    while (q.next()) {
      filters_in_feeds.insert(q.value(1).toString(), q.value(0).toInt());
    }

    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }

  return filters_in_feeds;
}

void DatabaseQueries::assignMessageFilterToFeed(const QSqlDatabase& db,
                                                const QString& feed_custom_id,
                                                int filter_id,
                                                int account_id,
                                                bool* ok) {
  QSqlQuery q(db);

  q.prepare(QSL("SELECT COUNT(*) FROM MessageFiltersInFeeds "
                "WHERE filter = :filter AND feed_custom_id = :feed_custom_id AND account_id = :account_id;"));
  q.setForwardOnly(true);
  q.bindValue(QSL(":filter"), filter_id);
  q.bindValue(QSL(":feed_custom_id"), feed_custom_id);
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec() && q.next()) {
    auto already_included_count = q.value(0).toInt();

    if (already_included_count > 0) {
      if (ok != nullptr) {
        *ok = true;
      }

      return;
    }
  }

  q.prepare(QSL("INSERT INTO MessageFiltersInFeeds (filter, feed_custom_id, account_id) "
                "VALUES(:filter, :feed_custom_id, :account_id);"));
  q.bindValue(QSL(":filter"), filter_id);
  q.bindValue(QSL(":feed_custom_id"), feed_custom_id);
  q.bindValue(QSL(":account_id"), account_id);

  if (q.exec()) {
    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }
}

void DatabaseQueries::updateMessageFilter(const QSqlDatabase& db, MessageFilter* filter, bool* ok) {
  QSqlQuery q(db);

  q.prepare(QSL("UPDATE MessageFilters SET name = :name, script = :script WHERE id = :id;"));

  q.bindValue(QSL(":name"), filter->name());
  q.bindValue(QSL(":script"), filter->script());
  q.bindValue(QSL(":id"), filter->id());
  q.setForwardOnly(true);

  if (q.exec()) {
    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }
}

void DatabaseQueries::removeMessageFilterFromFeed(const QSqlDatabase& db,
                                                  const QString& feed_custom_id,
                                                  int filter_id,
                                                  int account_id,
                                                  bool* ok) {
  QSqlQuery q(db);

  q.prepare(QSL("DELETE FROM MessageFiltersInFeeds "
                "WHERE filter = :filter AND feed_custom_id = :feed_custom_id AND account_id = :account_id;"));

  q.bindValue(QSL(":filter"), filter_id);
  q.bindValue(QSL(":feed_custom_id"), feed_custom_id);
  q.bindValue(QSL(":account_id"), account_id);
  q.setForwardOnly(true);

  if (q.exec()) {
    if (ok != nullptr) {
      *ok = true;
    }
  }
  else {
    if (ok != nullptr) {
      *ok = false;
    }
  }
}

QStringList DatabaseQueries::getAllGmailRecipients(const QSqlDatabase& db, int account_id) {
  QSqlQuery query(db);
  QStringList rec;

  query.prepare(QSL("SELECT DISTINCT author "
                    "FROM Messages "
                    "WHERE account_id = :account_id AND author IS NOT NULL AND author != '' "
                    "ORDER BY lower(author) ASC;"));
  query.bindValue(QSL(":account_id"), account_id);

  if (query.exec()) {
    while (query.next()) {
      rec.append(query.value(0).toString());
    }
  }
  else {
    qWarningNN << LOGSEC_GMAIL << "Query for all recipients failed: '" << query.lastError().text() << "'.";
  }

  return rec;
}

bool DatabaseQueries::storeNewOauthTokens(const QSqlDatabase& db, const QString& refresh_token, int account_id) {
  QSqlQuery query(db);

  query.prepare(QSL("SELECT custom_data FROM Accounts WHERE id = :id;"));
  query.bindValue(QSL(":id"), account_id);

  if (!query.exec() || !query.next()) {
    qWarningNN << LOGSEC_OAUTH << "Cannot fetch custom data column for storing of OAuth tokens, because of error:"
               << QUOTE_W_SPACE_DOT(query.lastError().text());
    return false;
  }

  QVariantHash custom_data = deserializeCustomData(query.value(0).toString());

  custom_data[QSL("refresh_token")] = refresh_token;

  query.clear();
  query.prepare(QSL("UPDATE Accounts SET custom_data = :custom_data WHERE id = :id;"));
  query.bindValue(QSL(":custom_data"), serializeCustomData(custom_data));
  query.bindValue(QSL(":id"), account_id);

  if (!query.exec()) {
    qWarningNN << LOGSEC_OAUTH
               << "Cannot store OAuth tokens, because of error:" << QUOTE_W_SPACE_DOT(query.lastError().text());
    return false;
  }
  else {
    return true;
  }
}

QString DatabaseQueries::unnulifyString(const QString& str) {
  return str.isNull() ? QSL("") : str;
}
