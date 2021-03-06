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
#ifndef QV4LOOKUP_H
#define QV4LOOKUP_H

#include "qv4global_p.h"
#include "qv4runtime_p.h"
#include "qv4engine_p.h"
#include "qv4context_p.h"
#include "qv4object_p.h"
#include "qv4internalclass_p.h"

QT_BEGIN_NAMESPACE

namespace QV4 {

struct Lookup {
    enum { Size = 4 };
    union {
        ReturnedValue (*getter)(Lookup *l, const ValueRef object);
        ReturnedValue (*globalGetter)(Lookup *l, ExecutionContext *ctx);
        void (*setter)(Lookup *l, const ValueRef object, const ValueRef v);
    };
    union {
        InternalClass *classList[Size];
        struct {
            void *dummy0;
            void *dummy1;
            Object *proto;
            unsigned type;
        };
    };
    int level;
    uint index;
    String *name;

    static ReturnedValue getterGeneric(Lookup *l, const ValueRef object);
    static ReturnedValue getter0(Lookup *l, const ValueRef object);
    static ReturnedValue getter1(Lookup *l, const ValueRef object);
    static ReturnedValue getter2(Lookup *l, const ValueRef object);
    static ReturnedValue getterAccessor0(Lookup *l, const ValueRef object);
    static ReturnedValue getterAccessor1(Lookup *l, const ValueRef object);
    static ReturnedValue getterAccessor2(Lookup *l, const ValueRef object);

    static ReturnedValue primitiveGetter0(Lookup *l, const ValueRef object);
    static ReturnedValue primitiveGetter1(Lookup *l, const ValueRef object);
    static ReturnedValue primitiveGetterAccessor0(Lookup *l, const ValueRef object);
    static ReturnedValue primitiveGetterAccessor1(Lookup *l, const ValueRef object);
    static ReturnedValue stringLengthGetter(Lookup *l, const ValueRef object);

    static ReturnedValue globalGetterGeneric(Lookup *l, ExecutionContext *ctx);
    static ReturnedValue globalGetter0(Lookup *l, ExecutionContext *ctx);
    static ReturnedValue globalGetter1(Lookup *l, ExecutionContext *ctx);
    static ReturnedValue globalGetter2(Lookup *l, ExecutionContext *ctx);
    static ReturnedValue globalGetterAccessor0(Lookup *l, ExecutionContext *ctx);
    static ReturnedValue globalGetterAccessor1(Lookup *l, ExecutionContext *ctx);
    static ReturnedValue globalGetterAccessor2(Lookup *l, ExecutionContext *ctx);

    static void setterGeneric(Lookup *l, const ValueRef object, const ValueRef value);
    static void setter0(Lookup *l, const ValueRef object, const ValueRef value);
    static void setterInsert0(Lookup *l, const ValueRef object, const ValueRef value);
    static void setterInsert1(Lookup *l, const ValueRef object, const ValueRef value);
    static void setterInsert2(Lookup *l, const ValueRef object, const ValueRef value);

    Property *lookup(Object *obj, PropertyAttributes *attrs);

};

}

QT_END_NAMESPACE

#endif
