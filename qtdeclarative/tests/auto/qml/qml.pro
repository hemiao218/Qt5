TEMPLATE = subdirs

METATYPETESTS += \
    qqmlmetatype

PUBLICTESTS += \
    parserstress \
    qjsvalueiterator \
    qjsonbinding \
    qmlmin \
    qmlplugindump \
    qqmlcomponent \
    qqmlconsole \
    qqmlengine \
    qqmlerror \
    qqmlincubator \
    qqmlinfo \
    qqmllistreference \
    qqmllocale \
    qqmlmetaobject \
    qqmlmoduleplugin \
    qqmlnotifier \
    qqmlqt \
    qqmltranslation \
    qqmlxmlhttprequest \
    qtqmlmodules \
    qquickfolderlistmodel \
    qqmlapplicationengine \
    qqmlsettings

PRIVATETESTS += \
    animation \
    qqmlcpputils \
    qqmlecmascript \
    qqmlcontext \
    qqmlexpression \
    qqmldirparser \
    qqmlglobal \
    qqmlinstruction \
    qqmllanguage \
    qqmlproperty \
    qqmlpropertycache \
    qqmlpropertymap \
    qqmlsqldatabase \
    qqmlvaluetypes \
    qqmlvaluetypeproviders \
    qqmlbinding \
    qqmlchangeset \
    qqmlconnections \
    qqmllistcompositor \
    qqmllistmodel \
    qqmllistmodelworkerscript \
    qqmltypeloader \
    qqmlparser \
    qquickworkerscript \
    qqmlbundle \
    qrcqml \
    qqmltimer \
    qqmlinstantiator \
    qv4debugger \
    qqmlenginecleanup

qtHaveModule(widgets) {
    PUBLICTESTS += \
        qjsengine \
        qjsvalue
}

SUBDIRS += $$PUBLICTESTS
SUBDIRS += $$METATYPETESTS
SUBDIRS += debugger

contains(QT_CONFIG, private_tests) {
    SUBDIRS += $$PRIVATETESTS
}

qtNomakeTools( \
    qmlplugindump \
)
