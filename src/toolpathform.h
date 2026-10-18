#pragma once
#include <QHash>
#include <QJsonObject>
#include <QWidget>

class QLabel;
class QLineEdit;
class QVBoxLayout;

namespace c2d {

class Document;

// The selected toolpath's settings as a form instead of a key/value table:
// a header (name, enabled), the tool as a card, then grouped cards - Depth,
// Cutting, Entry, Tabs, Rest, Finishing - holding only the fields this
// toolpath type has, each with a real editor: a switch for on/off, a number
// field with its unit and arrows, a dropdown for a choice. Every key without
// a designed field is still editable in the folded Advanced card.
//
// Values keep their JSON type (CC stores depths as strings, rates as
// numbers, flags as bools); depths are shown positive and written back with
// the sign convention the file already uses.
class ToolpathForm : public QWidget
{
    Q_OBJECT
public:
    explicit ToolpathForm(QWidget *parent = nullptr);

    // Show `json` (toolpath `uuid` of `type`). The same toolpath with the
    // same keys only updates the values, so focus, scroll and a held spin
    // arrow survive the refresh that follows every edit.
    void setToolpath(const QString &uuid, const QString &type, const QJsonObject &json);
    void clear();
    // Number fields evaluate expressions against this document's parameters.
    void setDocument(const Document *doc) { m_doc = doc; }

    struct Field;   // one designed field (toolpathform.cpp)

signals:
    void edited(const QJsonObject &json);   // the whole payload, one change applied
    void changeToolRequested();

private:
    void rebuild();
    void updateValues();
    void commit(const QString &key, const QJsonValue &v);
    // Set `key` and its expression binding (empty object: unbound) at once.
    void commitBound(const QString &key, const QJsonValue &v, const QJsonObject &binding);
    QString bindingOf(const QString &key) const;
    QWidget *card(const QString &id, const QString &title, bool openByDefault, QVBoxLayout **body);
    QWidget *editorFor(const QString &key, const Field &f);
    void refreshEnabled();

    const Document *m_doc = nullptr;
    QString m_uuid, m_type;
    QJsonObject m_json;
    QStringList m_keys;                    // flat keys of m_json, the rebuild signature
    QVBoxLayout *m_lay;
    QHash<QString, QWidget *> m_editors;   // flat key -> editor, for updateValues()
    QHash<QString, QString> m_enabledBy;   // flat key -> bool key that greys it out
    QLabel *m_toolIcon = nullptr;
    QLabel *m_toolText = nullptr;
    QLineEdit *m_name = nullptr;
    bool m_loading = false;
};

} // namespace c2d
