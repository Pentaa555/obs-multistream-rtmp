// SPDX-License-Identifier: GPL-2.0-or-later

#include "multistream-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QDockWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QString>

#include <memory>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multistream-rtmp", "en-US")
OBS_MODULE_AUTHOR("OBS Multistream RTMP contributors")

namespace {
constexpr const char *kDockId = "obs-multistream-rtmp-dock";
std::unique_ptr<multistream::MultistreamDock> dock;
QAction *open_dock_action = nullptr;

QDockWidget *find_dock_shell(QWidget *content)
{
    for (QWidget *parent = content ? content->parentWidget() : nullptr; parent; parent = parent->parentWidget()) {
        if (auto *dock_widget = qobject_cast<QDockWidget *>(parent))
            return dock_widget;
    }
    return nullptr;
}

QString normalized_action_text(QString value)
{
    value.remove('&');
    return value.simplified().toLower();
}

void remove_dock_from_panels(QMainWindow *main_window, QDockWidget *dock_widget)
{
    if (!main_window || !dock_widget)
        return;

    QAction *toggle_action = dock_widget->toggleViewAction();
    for (QMenu *menu : main_window->findChildren<QMenu *>()) {
        if (menu->actions().contains(toggle_action))
            menu->removeAction(toggle_action);
    }
}

void add_open_dock_action(QMainWindow *main_window)
{
    if (!main_window || open_dock_action)
        return;

    auto *menu_bar = main_window->menuBar();
    open_dock_action = new QAction(QString::fromUtf8(obs_module_text("DockTitle")), main_window);
    open_dock_action->setObjectName("obs-multistream-rtmp-open-action");
    open_dock_action->setToolTip(QString::fromUtf8(obs_module_text("DockTitle")));
    QObject::connect(open_dock_action, &QAction::triggered, [](bool) {
        if (dock)
            dock->open_dock();
    });

    QAction *tools_action = nullptr;
    for (QAction *action : menu_bar->actions()) {
        QMenu *menu = action->menu();
        if (!menu)
            continue;
        const QString action_name = normalized_action_text(action->text());
        const QString menu_name = normalized_action_text(menu->objectName() + " " + menu->title());
        if (menu_name.contains("menutools") || menu_name.contains("tools") || menu_name.contains("herramientas") ||
            action_name == "tools" || action_name == "herramientas") {
            tools_action = action;
            break;
        }
    }

    if (tools_action)
        menu_bar->insertAction(tools_action, open_dock_action);
    else
        menu_bar->addAction(open_dock_action);
}
}

bool obs_module_load(void)
{
    if (!obs_frontend_get_main_window())
        return false;

    dock = std::make_unique<multistream::MultistreamDock>();
    if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("DockTitle"), dock.get())) {
        dock.reset();
        return false;
    }

    if (QMainWindow *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window())) {
        if (QDockWidget *dock_widget = find_dock_shell(dock.get())) {
            dock_widget->setMinimumSize(720, 440);
            dock_widget->hide();
            remove_dock_from_panels(main_window, dock_widget);
        }
        dock->hide();
        add_open_dock_action(main_window);
    } else {
        dock->hide();
    }

    blog(LOG_INFO, "obs-multistream-rtmp %s loaded", PLUGIN_VERSION);
    return true;
}

void obs_module_unload(void)
{
    if (open_dock_action) {
        if (auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window()))
            main_window->menuBar()->removeAction(open_dock_action);
        delete open_dock_action;
        open_dock_action = nullptr;
    }
    obs_frontend_remove_dock(kDockId);
    dock.reset();
}

const char *obs_module_description(void)
{
    return "Local multi-destination RTMP output for OBS Studio";
}
