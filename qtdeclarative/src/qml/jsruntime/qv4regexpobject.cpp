/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3.0 as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU General Public License version 3.0 requirements will be
** met: http://www.gnu.org/copyleft/gpl.html.
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qv4regexpobject_p.h"
#include "qv4jsir_p.h"
#include "qv4isel_p.h"
#include "qv4objectproto_p.h"
#include "qv4stringobject_p.h"
#include "qv4mm_p.h"
#include "qv4scopedvalue_p.h"

#include <private/qqmljsengine_p.h>
#include <private/qqmljslexer_p.h>
#include <private/qqmljsparser_p.h>
#include <private/qqmljsast_p.h>
#include <qv4jsir_p.h>
#include <qv4codegen_p.h>
#include "private/qlocale_tools_p.h"

#include <QtCore/qmath.h>
#include <QtCore/QDebug>
#include <QtCore/qregexp.h>
#include <cassert>
#include <typeinfo>
#include <iostream>
#include "qv4alloca_p.h"

QT_BEGIN_NAMESPACE

Q_CORE_EXPORT QString qt_regexp_toCanonical(const QString &, QRegExp::PatternSyntax);

using namespace QV4;

DEFINE_MANAGED_VTABLE(RegExpObject);

RegExpObject::RegExpObject(InternalClass *ic)
    : Object(ic)
    , value(RegExp::create(ic->engine, QString(), false, false))
    , global(false)
{
    init(ic->engine);
}

RegExpObject::RegExpObject(ExecutionEngine *engine, Referenced<RegExp> value, bool global)
    : Object(engine->regExpClass)
    , value(value)
    , global(global)
{
    init(engine);
}

// Converts a QRegExp to a JS RegExp.
// The conversion is not 100% exact since ECMA regexp and QRegExp
// have different semantics/flags, but we try to do our best.
RegExpObject::RegExpObject(ExecutionEngine *engine, const QRegExp &re)
    : Object(engine->regExpClass)
    , value(0)
    , global(false)
{
    // Convert the pattern to a ECMAScript pattern.
    QString pattern = QT_PREPEND_NAMESPACE(qt_regexp_toCanonical)(re.pattern(), re.patternSyntax());
    if (re.isMinimal()) {
        QString ecmaPattern;
        int len = pattern.length();
        ecmaPattern.reserve(len);
        int i = 0;
        const QChar *wc = pattern.unicode();
        bool inBracket = false;
        while (i < len) {
            QChar c = wc[i++];
            ecmaPattern += c;
            switch (c.unicode()) {
            case '?':
            case '+':
            case '*':
            case '}':
                if (!inBracket)
                    ecmaPattern += QLatin1Char('?');
                break;
            case '\\':
                if (i < len)
                    ecmaPattern += wc[i++];
                break;
            case '[':
                inBracket = true;
                break;
            case ']':
                inBracket = false;
                break;
            default:
                break;
            }
        }
        pattern = ecmaPattern;
    }

    Scope scope(engine);
    ScopedObject protectThis(scope, this);

    value = RegExp::create(engine, pattern, re.caseSensitivity() == Qt::CaseInsensitive, false);

    init(engine);
}

void RegExpObject::init(ExecutionEngine *engine)
{
    setVTable(&static_vtbl);
    type = Type_RegExpObject;

    Scope scope(engine);
    ScopedObject protectThis(scope, this);

    ScopedString lastIndex(scope, engine->newIdentifier(QStringLiteral("lastIndex")));
    Property *lastIndexProperty = insertMember(lastIndex, Attr_NotEnumerable|Attr_NotConfigurable);
    lastIndexProperty->value = Primitive::fromInt32(0);
    if (!this->value)
        return;

    QString p = this->value->pattern();
    if (p.isEmpty()) {
        p = QStringLiteral("(?:)");
    } else {
        // escape certain parts, see ch. 15.10.4
        p.replace('/', QLatin1String("\\/"));
    }

    ScopedValue v(scope);
    defineReadonlyProperty(QStringLiteral("source"), (v = engine->newString(p)));
    defineReadonlyProperty(QStringLiteral("global"), Primitive::fromBoolean(global));
    defineReadonlyProperty(QStringLiteral("ignoreCase"), Primitive::fromBoolean(this->value->ignoreCase()));
    defineReadonlyProperty(QStringLiteral("multiline"), Primitive::fromBoolean(this->value->multiLine()));
}


void RegExpObject::destroy(Managed *that)
{
    static_cast<RegExpObject *>(that)->~RegExpObject();
}

void RegExpObject::markObjects(Managed *that, ExecutionEngine *e)
{
    RegExpObject *re = static_cast<RegExpObject*>(that);
    if (re->value)
        re->value->mark(e);
    Object::markObjects(that, e);
}

Property *RegExpObject::lastIndexProperty(ExecutionContext *ctx)
{
    Q_UNUSED(ctx);
    Q_ASSERT(0 == internalClass->find(ctx->engine->newIdentifier(QStringLiteral("lastIndex"))));
    return &memberData[0];
}

// Converts a JS RegExp to a QRegExp.
// The conversion is not 100% exact since ECMA regexp and QRegExp
// have different semantics/flags, but we try to do our best.
QRegExp RegExpObject::toQRegExp() const
{
    Qt::CaseSensitivity caseSensitivity = value->ignoreCase() ? Qt::CaseInsensitive : Qt::CaseSensitive;
    return QRegExp(value->pattern(), caseSensitivity, QRegExp::RegExp2);
}

QString RegExpObject::toString() const
{
    QString result = QLatin1Char('/') + source();
    result += QLatin1Char('/');
    if (global)
        result += QLatin1Char('g');
    if (value->ignoreCase())
        result += QLatin1Char('i');
    if (value->multiLine())
        result += QLatin1Char('m');
    return result;
}

QString RegExpObject::source() const
{
    Scope scope(engine());
    ScopedString source(scope, scope.engine->newIdentifier(QStringLiteral("source")));
    ScopedValue s(scope, const_cast<RegExpObject *>(this)->get(source));
    return s->toQString();
}

uint RegExpObject::flags() const
{
    uint f = 0;
    if (global)
        f |= QV4::RegExpObject::RegExp_Global;
    if (value->ignoreCase())
        f |= QV4::RegExpObject::RegExp_IgnoreCase;
    if (value->multiLine())
        f |= QV4::RegExpObject::RegExp_Multiline;
    return f;
}

DEFINE_MANAGED_VTABLE(RegExpCtor);

RegExpCtor::RegExpCtor(ExecutionContext *scope)
    : FunctionObject(scope, QStringLiteral("RegExp"))
{
    setVTable(&static_vtbl);
}

ReturnedValue RegExpCtor::construct(Managed *m, CallData *callData)
{
    ExecutionContext *ctx = m->engine()->currentContext();
    Scope scope(ctx);

    ScopedValue r(scope, callData->argument(0));
    ScopedValue f(scope, callData->argument(1));
    Scoped<RegExpObject> re(scope, r);
    if (re) {
        if (!f->isUndefined())
            return ctx->throwTypeError();

        Scoped<RegExp> newRe(scope, re->value);
        return Encode(ctx->engine->newRegExpObject(newRe, re->global));
    }

    QString pattern;
    if (!r->isUndefined())
        pattern = r->toString(ctx)->toQString();
    if (scope.hasException())
        return Encode::undefined();

    bool global = false;
    bool ignoreCase = false;
    bool multiLine = false;
    if (!f->isUndefined()) {
        f = __qmljs_to_string(ctx, f);
        if (scope.hasException())
            return Encode::undefined();
        QString str = f->stringValue()->toQString();
        for (int i = 0; i < str.length(); ++i) {
            if (str.at(i) == QLatin1Char('g') && !global) {
                global = true;
            } else if (str.at(i) == QLatin1Char('i') && !ignoreCase) {
                ignoreCase = true;
            } else if (str.at(i) == QLatin1Char('m') && !multiLine) {
                multiLine = true;
            } else {
                return ctx->throwSyntaxError(QStringLiteral("Invalid flags supplied to RegExp constructor"));
            }
        }
    }

    Scoped<RegExp> regexp(scope, RegExp::create(ctx->engine, pattern, ignoreCase, multiLine));
    if (!regexp->isValid())
        return ctx->throwSyntaxError(QStringLiteral("Invalid regular expression"));

    return Encode(ctx->engine->newRegExpObject(regexp, global));
}

ReturnedValue RegExpCtor::call(Managed *that, CallData *callData)
{
    if (callData->argc > 0 && callData->args[0].as<RegExpObject>()) {
        if (callData->argc == 1 || callData->args[1].isUndefined())
            return callData->args[0].asReturnedValue();
    }

    return construct(that, callData);
}

void RegExpPrototype::init(ExecutionEngine *engine, ObjectRef ctor)
{
    Scope scope(engine);
    ScopedObject o(scope);

    ctor->defineReadonlyProperty(engine->id_prototype, (o = this));
    ctor->defineReadonlyProperty(engine->id_length, Primitive::fromInt32(2));
    defineDefaultProperty(QStringLiteral("constructor"), (o = ctor));
    defineDefaultProperty(QStringLiteral("exec"), method_exec, 1);
    defineDefaultProperty(QStringLiteral("test"), method_test, 1);
    defineDefaultProperty(engine->id_toString, method_toString, 0);
    defineDefaultProperty(QStringLiteral("compile"), method_compile, 2);
}

ReturnedValue RegExpPrototype::method_exec(CallContext *ctx)
{
    Scope scope(ctx);
    Scoped<RegExpObject> r(scope, ctx->callData->thisObject.as<RegExpObject>());
    if (!r)
        return ctx->throwTypeError();

    ScopedValue arg(scope, ctx->argument(0));
    arg = __qmljs_to_string(ctx, arg);
    if (scope.hasException())
        return Encode::undefined();
    QString s = arg->stringValue()->toQString();

    int offset = r->global ? r->lastIndexProperty(ctx)->value.toInt32() : 0;
    if (offset < 0 || offset > s.length()) {
        r->lastIndexProperty(ctx)->value = Primitive::fromInt32(0);
        return Encode::null();
    }

    uint* matchOffsets = (uint*)alloca(r->value->captureCount() * 2 * sizeof(uint));
    int result = r->value->match(s, offset, matchOffsets);
    if (result == -1) {
        r->lastIndexProperty(ctx)->value = Primitive::fromInt32(0);
        return Encode::null();
    }

    // fill in result data
    Scoped<ArrayObject> array(scope, ctx->engine->newArrayObject(ctx->engine->regExpExecArrayClass));
    int len = r->value->captureCount();
    array->arrayReserve(len);
    for (int i = 0; i < len; ++i) {
        int start = matchOffsets[i * 2];
        int end = matchOffsets[i * 2 + 1];
        array->arrayData[i].value = (start != -1 && end != -1) ? ctx->engine->newString(s.mid(start, end - start))->asReturnedValue() : Encode::undefined();
        array->arrayDataLen = i + 1;
    }
    array->setArrayLengthUnchecked(len);

    array->memberData[Index_ArrayIndex].value = Primitive::fromInt32(result);
    array->memberData[Index_ArrayInput].value = arg.asReturnedValue();

    if (r->global)
        r->lastIndexProperty(ctx)->value = Primitive::fromInt32(matchOffsets[1]);

    return array.asReturnedValue();
}

ReturnedValue RegExpPrototype::method_test(CallContext *ctx)
{
    Scope scope(ctx);
    ScopedValue r(scope, method_exec(ctx));
    return Encode(!r->isNull());
}

ReturnedValue RegExpPrototype::method_toString(CallContext *ctx)
{
    Scope scope(ctx);
    Scoped<RegExpObject> r(scope, ctx->callData->thisObject.as<RegExpObject>());
    if (!r)
        return ctx->throwTypeError();

    return ctx->engine->newString(r->toString())->asReturnedValue();
}

ReturnedValue RegExpPrototype::method_compile(CallContext *ctx)
{
    Scope scope(ctx);
    Scoped<RegExpObject> r(scope, ctx->callData->thisObject.as<RegExpObject>());
    if (!r)
        return ctx->throwTypeError();

    ScopedCallData callData(scope, ctx->callData->argc);
    memcpy(callData->args, ctx->callData->args, ctx->callData->argc*sizeof(SafeValue));

    Scoped<RegExpObject> re(scope, ctx->engine->regExpCtor.asFunctionObject()->construct(callData));

    r->value = re->value;
    r->global = re->global;
    return Encode::undefined();
}

QT_END_NAMESPACE
