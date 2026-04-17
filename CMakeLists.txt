cmake_minimum_required(VERSION 3.20)
project(soundroot-plasmoid VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# ── KDE ECM ──────────────────────────────────────────────────────────────
find_package(ECM REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

# ── Qt 6 ─────────────────────────────────────────────────────────────────
set(QT_DEFAULT_MAJOR_VERSION 6)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Quick)

# ── KDE Frameworks 6 ────────────────────────────────────────────────────
find_package(KF6 REQUIRED COMPONENTS CoreAddons Config Package GlobalAccel I18n XmlGui)

# ── Plasma 6 ─────────────────────────────────────────────────────────────
find_package(Plasma REQUIRED)

# ── PulseAudio ───────────────────────────────────────────────────────────
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED IMPORTED_TARGET libpulse)

# ══════════════════════════════════════════════════════════════════════════
# 1. Install the QML package (metadata.json + contents/)
# ══════════════════════════════════════════════════════════════════════════
plasma_install_package(package org.kde.plasma.soundroot)

# ══════════════════════════════════════════════════════════════════════════
# 2. C++ plugin library (extends Plasma::Applet → plasmoid.nativeInterface)
# ══════════════════════════════════════════════════════════════════════════
set(PLUGIN_SOURCES
    plugin/audiorouterapplet.cpp
    plugin/audiorouterapplet.h
    backend/audiobackend.cpp
    backend/audiobackend.h
    backend/data/audiogroup.cpp
    backend/data/audiogroup.h
    backend/data/settings.cpp
    backend/data/settings.h
    backend/models/grouplistmodel.cpp
    backend/models/grouplistmodel.h
    backend/models/routelistmodel.cpp
    backend/models/routelistmodel.h
    backend/models/sinkinputmodel.cpp
    backend/models/sinkinputmodel.h
    backend/models/sinkmodel.cpp
    backend/models/sinkmodel.h
)

set(SOUNDROOT_PLUGIN_TARGET plasma_applet_soundroot)
set(SOUNDROOT_PLUGIN_OUTPUT_NAME org.kde.plasma.soundroot)

add_library(${SOUNDROOT_PLUGIN_TARGET} MODULE ${PLUGIN_SOURCES})

set_target_properties(${SOUNDROOT_PLUGIN_TARGET} PROPERTIES
    OUTPUT_NAME ${SOUNDROOT_PLUGIN_OUTPUT_NAME}
)

target_include_directories(${SOUNDROOT_PLUGIN_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/plugin
    ${CMAKE_CURRENT_SOURCE_DIR}/backend
)

target_link_libraries(${SOUNDROOT_PLUGIN_TARGET} PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    KF6::CoreAddons
    KF6::Package
    KF6::GlobalAccel
    KF6::I18n
    KF6::XmlGui
    Plasma::Plasma
    PkgConfig::PULSE
)

install(TARGETS ${SOUNDROOT_PLUGIN_TARGET}
        DESTINATION ${KDE_INSTALL_PLUGINDIR}/plasma/applets)
