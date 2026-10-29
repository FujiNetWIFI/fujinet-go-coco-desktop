/*
 * Settings dialog for the KDE/Qt frontend.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QLineEdit>

#include <vector>

#include "cocosession.h"

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(cocosession *session, QWidget *parent = nullptr);

    /* True when one of the start-time options (FujiNet / audio / gamepads)
     * changed, so the caller knows the session has to be restarted for it to
     * take effect. Machine and video options are NOT among them: on this
     * target they all switch in place, from the Machine and View menus. */
    bool sessionDirty() const { return m_sessionDirty; }

    /* Saves the external FujiNet fields first; an address that is not
     * host:port keeps the dialog open. */
    void done(int result) override;

signals:
    /* After an import from the ROMs tab (the caller may now start the machine). */
    void romsImported();
    /* Window-only options apply live. */
    void displayChanged();

private:
    void importRoms(bool folder);
    void refreshRoms();
    QCheckBox *addStartOption(class QFormLayout *form, const QString &label,
                              const char *key, int def);

    cocosession *m_session;
    QCheckBox *m_external = nullptr;
    QLineEdit *m_externalAddr = nullptr;
    bool m_sessionDirty = false;
    std::vector<class QLabel *> m_romRows;
};
