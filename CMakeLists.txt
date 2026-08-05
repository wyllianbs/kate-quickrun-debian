cmake_minimum_required(VERSION 3.16)

project(kate-quickrun VERSION 1.0 LANGUAGES CXX)

set(QT_MIN_VERSION "6.5.0")
set(KF_MIN_VERSION "6.0.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})

include(KDECompilerSettings NO_POLICY_SCOPE)
include(KDEInstallDirs)
include(KDECMakeSettings)

# Instruir o CMake a gerar automaticamente os arquivos MOC e UIC (.ui -> ui_*.h)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 ${QT_MIN_VERSION} REQUIRED COMPONENTS Core Widgets DBus)
find_package(KF6 ${KF_MIN_VERSION} REQUIRED COMPONENTS TextEditor I18n XmlGui Config Parts Service IconThemes)

# Domínio de tradução (gettext). i18n()/i18nc() no código usam este domínio.
add_definitions(-DTRANSLATION_DOMAIN="kate-quickrun")

set(plugin_SRCS
    kate-quickrun.cpp
    kate-quickrun-config.ui
    kate-quickrun.json
)

# Namespace moderno do KF6 para plugins do KTextEditor: instala em
# <qtplugins>/kf6/ktexteditor, o mesmo lugar dos plugins do próprio Kate.
kcoreaddons_add_plugin(kate-quickrun SOURCES ${plugin_SRCS} INSTALL_NAMESPACE "kf6/ktexteditor")

target_link_libraries(kate-quickrun
    PRIVATE
        Qt6::Core
        Qt6::Widgets
        Qt6::DBus
        KF6::TextEditor
        KF6::I18n
        KF6::XmlGui
        KF6::Parts
        KF6::Service
        KF6::IconWidgets
        KF6::ConfigCore
)

# Ícone próprio do plugin, no tema hicolor: fica disponível para QIcon::fromTheme
# e para o campo Icon do .json, sem depender de nenhum tema em particular.
install(FILES icons/quickrun.svg DESTINATION ${KDE_INSTALL_ICONDIR}/hicolor/scalable/apps)

# Traduções: compila e instala po/<lang>/kate-quickrun.po como catálogos .mo.
ki18n_install(po)