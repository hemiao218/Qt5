/****************************************************************************
**
** Copyright (C) 2013 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of the QtQuick module of the Qt Toolkit.
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

#include "qsgbatchrenderer_p.h"
#include <private/qsgshadersourcebuilder_p.h>

#include <QtCore/QElapsedTimer>

#include <QtGui/QGuiApplication>
#include <QtGui/QOpenGLFramebufferObject>
#include <QtGui/QOpenGLVertexArrayObject>

#include <private/qqmlprofilerservice_p.h>

#include <algorithm>

#ifndef GL_DOUBLE
   #define GL_DOUBLE 0x140A
#endif

QT_BEGIN_NAMESPACE

extern QByteArray qsgShaderRewriter_insertZAttributes(const char *input, QSurfaceFormat::OpenGLContextProfile profile);

namespace QSGBatchRenderer
{

const bool debug_render     = qgetenv("QSG_RENDERER_DEBUG").contains("render");
const bool debug_build      = qgetenv("QSG_RENDERER_DEBUG").contains("build");
const bool debug_change     = qgetenv("QSG_RENDERER_DEBUG").contains("change");
const bool debug_upload     = qgetenv("QSG_RENDERER_DEBUG").contains("upload");
const bool debug_roots      = qgetenv("QSG_RENDERER_DEBUG").contains("roots");
const bool debug_dump       = qgetenv("QSG_RENDERER_DEBUG").contains("dump");
const bool debug_noalpha    = qgetenv("QSG_RENDERER_DEBUG").contains("noalpha");
const bool debug_noopaque   = qgetenv("QSG_RENDERER_DEBUG").contains("noopaque");
const bool debug_noclip     = qgetenv("QSG_RENDERER_DEBUG").contains("noclip");

#ifndef QSG_NO_RENDER_TIMING
static bool qsg_render_timing = !qgetenv("QSG_RENDER_TIMING").isEmpty();
static QElapsedTimer qsg_renderer_timer;
#endif

#define QSGNODE_TRAVERSE(NODE) for (QSGNode *child = NODE->firstChild(); child; child = child->nextSibling())
#define SHADOWNODE_TRAVERSE(NODE) for (QList<Node *>::const_iterator child = NODE->children.constBegin(); child != NODE->children.constEnd(); ++child)

static inline int size_of_type(GLenum type)
{
    static int sizes[] = {
        sizeof(char),
        sizeof(unsigned char),
        sizeof(short),
        sizeof(unsigned short),
        sizeof(int),
        sizeof(unsigned int),
        sizeof(float),
        2,
        3,
        4,
        sizeof(double)
    };
    Q_ASSERT(type >= GL_BYTE && type <= 0x140A); // the value of GL_DOUBLE
    return sizes[type - GL_BYTE];
}

bool qsg_sort_element_increasing_order(Element *a, Element *b) { return a->order < b->order; }
bool qsg_sort_element_decreasing_order(Element *a, Element *b) { return a->order > b->order; }
bool qsg_sort_batch_is_valid(Batch *a, Batch *b) { return a->first && !b->first; }
bool qsg_sort_batch_increasing_order(Batch *a, Batch *b) { return a->first->order < b->first->order; }
bool qsg_sort_batch_decreasing_order(Batch *a, Batch *b) { return a->first->order > b->first->order; }

QSGMaterial::Flag QSGMaterial_RequiresFullMatrixBit = (QSGMaterial::Flag) (QSGMaterial::RequiresFullMatrix & ~QSGMaterial::RequiresFullMatrixExceptTranslate);

struct QMatrix4x4_Accessor
{
    float m[4][4];
    int flagBits;

    static bool isTranslate(const QMatrix4x4 &m) { return ((const QMatrix4x4_Accessor &) m).flagBits <= 0x1; }
    static bool isScale(const QMatrix4x4 &m) { return ((const QMatrix4x4_Accessor &) m).flagBits <= 0x2; }
    static bool is2DSafe(const QMatrix4x4 &m) { return ((const QMatrix4x4_Accessor &) m).flagBits < 0x8; }
};

const float OPAQUE_LIMIT                = 0.999f;

ShaderManager::Shader *ShaderManager::prepareMaterial(QSGMaterial *material)
{
    QSGMaterialType *type = material->type();
    Shader *shader = rewrittenShaders.value(type, 0);
    if (shader)
        return shader;

#ifndef QSG_NO_RENDER_TIMING
    if (qsg_render_timing  || QQmlProfilerService::enabled)
        qsg_renderer_timer.start();
#endif

    QSGMaterialShader *s = material->createShader();
    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    QSurfaceFormat::OpenGLContextProfile profile = ctx->format().profile();

    QOpenGLShaderProgram *p = s->program();
    p->addShaderFromSourceCode(QOpenGLShader::Vertex,
                               qsgShaderRewriter_insertZAttributes(s->vertexShader(), profile));
    p->addShaderFromSourceCode(QOpenGLShader::Fragment,
                               s->fragmentShader());

    char const *const *attr = s->attributeNames();
    int i;
    for (i = 0; attr[i]; ++i) {
        if (*attr[i])
            p->bindAttributeLocation(attr[i], i);
    }
    p->bindAttributeLocation("_qt_order", i);

    p->link();
    if (!p->isLinked()) {
        qDebug() << "Renderer failed shader compilation:" << endl << p->log();
        return 0;
    }

    s->initialize();

    shader = new Shader;
    shader->program = s;
    shader->pos_order = i;
    shader->id_zRange = p->uniformLocation("_qt_zRange");
    shader->lastOpacity = 0;

    Q_ASSERT(shader->pos_order >= 0);
    Q_ASSERT(shader->id_zRange >= 0);

#ifndef QSG_NO_RENDER_TIMING
    if (qsg_render_timing)
        qDebug("   - compiling material: %dms", (int) qsg_renderer_timer.elapsed());

    if (QQmlProfilerService::enabled) {
        QQmlProfilerService::sceneGraphFrame(
                    QQmlProfilerService::SceneGraphContextFrame,
                    qsg_renderer_timer.nsecsElapsed());
    }
#endif

    rewrittenShaders[type] = shader;
    return shader;
}

ShaderManager::Shader *ShaderManager::prepareMaterialNoRewrite(QSGMaterial *material)
{
    QSGMaterialType *type = material->type();
    Shader *shader = stockShaders.value(type, 0);
    if (shader)
        return shader;

#ifndef QSG_NO_RENDER_TIMING
    if (qsg_render_timing  || QQmlProfilerService::enabled)
        qsg_renderer_timer.start();
#endif

    QSGMaterialShader *s = static_cast<QSGMaterialShader *>(material->createShader());
    s->compile();
    s->initialize();

    shader = new Shader();
    shader->program = s;
    shader->id_zRange = -1;
    shader->pos_order = -1;
    shader->lastOpacity = 0;

    stockShaders[type] = shader;

#ifndef QSG_NO_RENDER_TIMING
    if (qsg_render_timing)
        qDebug("   - compiling material: %dms", (int) qsg_renderer_timer.elapsed());

    if (QQmlProfilerService::enabled) {
        QQmlProfilerService::sceneGraphFrame(
                    QQmlProfilerService::SceneGraphContextFrame,
                    qsg_renderer_timer.nsecsElapsed());
    }
#endif

    return shader;
}

void ShaderManager::invalidated()
{
    qDeleteAll(stockShaders.values());
    stockShaders.clear();
    qDeleteAll(rewrittenShaders.values());
    rewrittenShaders.clear();
    delete blitProgram;
    blitProgram = 0;
}

void qsg_dumpShadowRoots(BatchRootInfo *i, int indent)
{
    static int extraIndent = 0;
    ++extraIndent;

    QByteArray ind(indent + extraIndent + 10, ' ');

    if (!i) {
        qDebug() << ind.constData() << "- no info";
    } else {
        qDebug() << ind.constData() << "- parent:" << i->parentRoot << "orders" << i->firstOrder << "->" << i->lastOrder << ", avail:" << i->availableOrders;
        for (QSet<Node *>::const_iterator it = i->subRoots.constBegin();
             it != i->subRoots.constEnd(); ++it) {
            qDebug() << ind.constData() << "-" << *it;
            qsg_dumpShadowRoots((*it)->rootInfo(), indent);
        }
    }

    --extraIndent;
}

void qsg_dumpShadowRoots(Node *n)
{
    static int indent = 0;
    ++indent;

    QByteArray ind(indent, ' ');

    if (n->type() == QSGNode::ClipNodeType || n->isBatchRoot) {
        qDebug() << ind.constData() << "[X]" << n->sgNode << hex << uint(n->sgNode->flags());
        qsg_dumpShadowRoots(n->rootInfo(), indent);
    } else {
        QDebug d = qDebug();
        d << ind.constData() << "[ ]" << n->sgNode << hex << uint(n->sgNode->flags());
        if (n->type() == QSGNode::GeometryNodeType)
            d << "order" << dec << n->element()->order;
    }

    SHADOWNODE_TRAVERSE(n)
            qsg_dumpShadowRoots(*child);

    --indent;
}

Updater::Updater(Renderer *r)
    : renderer(r)
    , m_roots(32)
    , m_rootMatrices(8)
{
    m_roots.add(0);
    m_combined_matrix_stack.add(&m_identityMatrix);
    m_rootMatrices.add(m_identityMatrix);

    Q_ASSERT(sizeof(QMatrix4x4_Accessor) == sizeof(QMatrix4x4));
}

void Updater::updateStates(QSGNode *n)
{
    m_current_clip = 0;

    m_added = 0;
    m_transformChange = 0;

    Node *sn = renderer->m_nodes.value(n, 0);
    Q_ASSERT(sn);

    if (Q_UNLIKELY(debug_roots))
        qsg_dumpShadowRoots(sn);

    if (Q_UNLIKELY(debug_build)) {
        qDebug() << "Updater::updateStates()";
        if (sn->dirtyState & (QSGNode::DirtyNodeAdded << 16))
            qDebug() << " - nodes have been added";
        if (sn->dirtyState & (QSGNode::DirtyMatrix << 16))
            qDebug() << " - transforms have changed";
        if (sn->dirtyState & (QSGNode::DirtyOpacity << 16))
            qDebug() << " - opacity has changed";
        if (sn->dirtyState & (QSGNode::DirtyForceUpdate << 16))
            qDebug() << " - forceupdate";
    }

    visitNode(sn);
}

void Updater::visitNode(Node *n)
{
    if (m_added == 0 && n->dirtyState == 0 && m_force_update == 0 && m_transformChange == 0)
        return;

    int count = m_added;
    if (n->dirtyState & QSGNode::DirtyNodeAdded)
        ++m_added;

    int force = m_force_update;
    if (n->dirtyState & QSGNode::DirtyForceUpdate)
        ++m_force_update;

    switch (n->type()) {
    case QSGNode::OpacityNodeType:
        visitOpacityNode(n);
        break;
    case QSGNode::TransformNodeType:
        visitTransformNode(n);
        break;
    case QSGNode::GeometryNodeType:
        visitGeometryNode(n);
        break;
    case QSGNode::ClipNodeType:
        visitClipNode(n);
        break;
    case QSGNode::RenderNodeType:
        if (m_added)
            n->renderNodeElement()->root = m_roots.last();
        // Fall through to visit children.
    default:
        SHADOWNODE_TRAVERSE(n) visitNode(*child);
        break;
    }

    m_added = count;
    m_force_update = force;
    n->dirtyState = 0;
}

void Updater::visitClipNode(Node *n)
{
    ClipBatchRootInfo *extra = n->clipInfo();

    QSGClipNode *cn = static_cast<QSGClipNode *>(n->sgNode);

    if (m_roots.last() && m_added > 0)
        renderer->registerBatchRoot(n, m_roots.last());

    cn->m_clip_list = m_current_clip;
    m_current_clip = cn;
    m_roots << n;
    m_rootMatrices.add(m_rootMatrices.last() * *m_combined_matrix_stack.last());
    extra->matrix = m_rootMatrices.last();
    cn->m_matrix = &extra->matrix;
    m_combined_matrix_stack << &m_identityMatrix;

    SHADOWNODE_TRAVERSE(n) visitNode(*child);

    m_current_clip = cn->m_clip_list;
    m_rootMatrices.pop_back();
    m_combined_matrix_stack.pop_back();
    m_roots.pop_back();
}

void Updater::visitOpacityNode(Node *n)
{
    QSGOpacityNode *on = static_cast<QSGOpacityNode *>(n->sgNode);

    qreal combined = m_opacity_stack.last() * on->opacity();
    on->setCombinedOpacity(combined);
    m_opacity_stack.add(combined);

    if (m_added == 0 && n->dirtyState & QSGNode::DirtyOpacity) {
        bool was = n->isOpaque;
        bool is = on->opacity() > OPAQUE_LIMIT;
        if (was != is) {
            renderer->m_rebuild = Renderer::FullRebuild;
            n->isOpaque = is;
        } else if (!is) {
            renderer->invalidateAlphaBatchesForRoot(m_roots.last());
            renderer->m_rebuild |= Renderer::BuildBatches;
        }
        ++m_force_update;
        SHADOWNODE_TRAVERSE(n) visitNode(*child);
        --m_force_update;
    } else {
        if (m_added > 0)
            n->isOpaque = on->opacity() > OPAQUE_LIMIT;
        SHADOWNODE_TRAVERSE(n) visitNode(*child);
    }

    m_opacity_stack.pop_back();
}

void Updater::visitTransformNode(Node *n)
{
    bool popMatrixStack = false;
    bool popRootStack = false;
    bool dirty = n->dirtyState & QSGNode::DirtyMatrix;

    QSGTransformNode *tn = static_cast<QSGTransformNode *>(n->sgNode);

    if (n->isBatchRoot) {
        if (m_added > 0 && m_roots.last())
            renderer->registerBatchRoot(n, m_roots.last());
        tn->setCombinedMatrix(m_rootMatrices.last() * *m_combined_matrix_stack.last() * tn->matrix());

        // The only change in this subtree is ourselves and we are a batch root, so
        // only update subroots and return, saving tons of child-processing (flickable-panning)

        if (!n->becameBatchRoot && m_added == 0 && m_force_update == 0 && dirty && (n->dirtyState & ~QSGNode::DirtyMatrix) == 0) {
            BatchRootInfo *info = renderer->batchRootInfo(n);
            for (QSet<Node *>::const_iterator it = info->subRoots.constBegin();
                 it != info->subRoots.constEnd(); ++it) {
                updateRootTransforms(*it, n, tn->combinedMatrix());
            }
            return;
        }

        n->becameBatchRoot = false;

        m_combined_matrix_stack.add(&m_identityMatrix);
        m_roots.add(n);
        m_rootMatrices.add(tn->combinedMatrix());

        popMatrixStack = true;
        popRootStack = true;
    } else if (!tn->matrix().isIdentity()) {
        tn->setCombinedMatrix(*m_combined_matrix_stack.last() * tn->matrix());
        m_combined_matrix_stack.add(&tn->combinedMatrix());
        popMatrixStack = true;
    } else {
        tn->setCombinedMatrix(*m_combined_matrix_stack.last());
    }

    if (dirty)
        ++m_transformChange;

    SHADOWNODE_TRAVERSE(n) visitNode(*child);

    if (dirty)
        --m_transformChange;
    if (popMatrixStack)
        m_combined_matrix_stack.pop_back();
    if (popRootStack) {
        m_roots.pop_back();
        m_rootMatrices.pop_back();
    }
}

void Updater::visitGeometryNode(Node *n)
{
    QSGGeometryNode *gn = static_cast<QSGGeometryNode *>(n->sgNode);

    gn->m_matrix = m_combined_matrix_stack.last();
    gn->m_clip_list = m_current_clip;
    gn->setInheritedOpacity(m_opacity_stack.last());

    if (m_added) {
        Element *e = n->element();
        e->root = m_roots.last();
        e->translateOnlyToRoot = QMatrix4x4_Accessor::isTranslate(*gn->matrix());

        if (e->root) {
            BatchRootInfo *info = renderer->batchRootInfo(e->root);
            info->availableOrders--;
            if (info->availableOrders < 0) {
                renderer->m_rebuild |= Renderer::BuildRenderLists;
            } else {
                renderer->m_rebuild |= Renderer::BuildRenderListsForTaggedRoots;
                renderer->m_taggedRoots << e->root;
            }
        } else {
            renderer->m_rebuild |= Renderer::FullRebuild;
        }
    } else if (m_transformChange) {
        Element *e = n->element();
        e->translateOnlyToRoot = QMatrix4x4_Accessor::isTranslate(*gn->matrix());
    }

    SHADOWNODE_TRAVERSE(n) visitNode(*child);
}

void Updater::updateRootTransforms(Node *node, Node *root, const QMatrix4x4 &combined)
{
    BatchRootInfo *info = renderer->batchRootInfo(node);
    QMatrix4x4 m;
    Node *n = node;

    while (n != root) {
        if (n->type() == QSGNode::TransformNodeType)
            m = static_cast<QSGTransformNode *>(n->sgNode)->matrix() * m;
        n = n->parent;
    }

    m = combined * m;

    if (node->type() == QSGNode::ClipNodeType) {
        static_cast<ClipBatchRootInfo *>(info)->matrix = m;
    } else {
        Q_ASSERT(node->type() == QSGNode::TransformNodeType);
        static_cast<QSGTransformNode *>(node->sgNode)->setCombinedMatrix(m);
    }

    for (QSet<Node *>::const_iterator it = info->subRoots.constBegin();
         it != info->subRoots.constEnd(); ++it) {
        updateRootTransforms(*it, node, m);
    }
}

int qsg_positionAttribute(QSGGeometry *g) {
    int vaOffset = 0;
    for (int a=0; a<g->attributeCount(); ++a) {
        const QSGGeometry::Attribute &attr = g->attributes()[a];
        if (attr.isVertexCoordinate && attr.tupleSize == 2 && attr.type == GL_FLOAT) {
            return vaOffset;
        }
        vaOffset += attr.tupleSize * size_of_type(attr.type);
    }
    return -1;
}


void Rect::map(const QMatrix4x4 &matrix)
{
    const float *m = matrix.constData();
    if (QMatrix4x4_Accessor::isScale(matrix)) {
        tl.x = tl.x * m[0] + m[12];
        tl.y = tl.y * m[5] + m[13];
        br.x = br.x * m[0] + m[12];
        br.y = br.y * m[5] + m[13];
        if (tl.x > br.x)
            qSwap(tl.x, br.x);
        if (tl.y > br.y)
            qSwap(tl.y, br.y);
    } else {
        Pt mtl = tl;
        Pt mtr = { br.x, tl.y };
        Pt mbl = { tl.x, br.y };
        Pt mbr = br;

        mtl.map(matrix);
        mtr.map(matrix);
        mbl.map(matrix);
        mbr.map(matrix);

        set(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
        (*this) |= mtl;
        (*this) |= mtr;
        (*this) |= mbl;
        (*this) |= mbr;
    }
}

void Element::computeBounds()
{
    Q_ASSERT(!boundsComputed);
    boundsComputed = true;

    QSGGeometry *g = node->geometry();
    int offset = qsg_positionAttribute(g);
    if (offset == -1) {
        // No position attribute means overlaps with everything..
        bounds.set(-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX);
        return;
    }

    bounds.set(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    char *vd = (char *) g->vertexData() + offset;
    for (int i=0; i<g->vertexCount(); ++i) {
        bounds |= *(Pt *) vd;
        vd += g->sizeOfVertex();
    }
    bounds.map(*node->matrix());

    if (!qIsFinite(bounds.tl.x) || bounds.tl.x == FLT_MAX)
        bounds.tl.x = -FLT_MAX;
    if (!qIsFinite(bounds.tl.y) || bounds.tl.y == FLT_MAX)
        bounds.tl.y = -FLT_MAX;
    if (!qIsFinite(bounds.br.x) || bounds.br.x == -FLT_MAX)
        bounds.br.x = FLT_MAX;
    if (!qIsFinite(bounds.br.y) || bounds.br.y == -FLT_MAX)
        bounds.br.y = FLT_MAX;

    Q_ASSERT(bounds.tl.x <= bounds.br.x);
    Q_ASSERT(bounds.tl.y <= bounds.br.y);

    boundsOutsideFloatRange = bounds.isOutsideFloatRange();
}

RenderNodeElement::~RenderNodeElement()
{
    delete fbo;
}

bool Batch::isMaterialCompatible(Element *e) const
{
    // If material has changed between opaque and translucent, it is not compatible
    QSGMaterial *m = e->node->activeMaterial();
    if (isOpaque != ((m->flags() & QSGMaterial::Blending) == 0))
        return false;

    Element *n = first;
    // Skip to the first node other than e which has not been removed
    while (n && (n == e || n->removed))
        n = n->nextInBatch;

    // Only 'e' in this batch, so a material change doesn't change anything as long as
    // its blending is still in sync with this batch...
    if (!n)
        return true;

    QSGMaterial *nm = n->node->activeMaterial();
    return nm->type() == m->type() && nm->compare(m) == 0;
}

/*
 * Marks this batch as dirty or in the case where the geometry node has
 * changed to be incompatible with this batch, return false so that
 * the caller can mark the entire sg for a full rebuild...
 */
bool Batch::geometryWasChanged(QSGGeometryNode *gn)
{
    Element *e = first;
    Q_ASSERT_X(e, "Batch::geometryWasChanged", "Batch is expected to 'valid' at this time");
    // 'gn' is the first node in the batch, compare against the next one.
    while (e && (e->node == gn || e->removed))
        e = e->nextInBatch;
    if (!e || e->node->geometry()->attributes() == gn->geometry()->attributes()) {
        needsUpload = true;
        return true;
    } else {
        return false;
    }
}

void Batch::cleanupRemovedElements()
{
    // remove from front of batch..
    while (first && first->removed) {
        first = first->nextInBatch;
    }

    // Then continue and remove other nodes further out in the batch..
    if (first) {
        Element *e = first;
        while (e->nextInBatch) {
            if (e->nextInBatch->removed)
                e->nextInBatch = e->nextInBatch->nextInBatch;
            else
                e = e->nextInBatch;

        }
    }
}

/*
 * Iterates through all geometry nodes in this batch and unsets their batch,
 * thus forcing them to be rebuilt
 */
void Batch::invalidate()
{
    // If doing removal here is a performance issue, we might add a "hasRemoved" bit to
    // the batch to do an early out..
    cleanupRemovedElements();
    Element *e = first;
    first = 0;
    root = 0;
    while (e) {
        e->batch = 0;
        Element *n = e->nextInBatch;
        e->nextInBatch = 0;
        e = n;
    }
}

bool Batch::isTranslateOnlyToRoot() const {
    bool only = true;
    Element *e = first;
    while (e && only) {
        only &= e->translateOnlyToRoot;
        e = e->nextInBatch;
    }
    return only;
}

/*
 * Iterates through all the nodes in the batch and returns true if the
 * nodes are all safe to batch. There are two separate criteria:
 *
 * - The matrix is such that the z component of the result is of no
 *   consequence.
 *
 * - The bounds are inside the stable floating point range. This applies
 *   to desktop only where we in this case can trigger a fallback to
 *   unmerged in which case we pass the geometry straight through and
 *   just apply the matrix.
 *
 *   NOTE: This also means a slight performance impact for geometries which
 *   are defined to be outside the stable floating point range and still
 *   use single precision float, but given that this implicitly fixes
 *   huge lists and tables, it is worth it.
 */
bool Batch::isSafeToBatch() const {
    Element *e = first;
    while (e) {
        if (e->boundsOutsideFloatRange)
            return false;
        if (!QMatrix4x4_Accessor::is2DSafe(*e->node->matrix()))
            return false;
        e = e->nextInBatch;
    }
    return true;
}

static int qsg_countNodesInBatch(const Batch *batch)
{
    int sum = 0;
    Element *e = batch->first;
    while (e) {
        ++sum;
        e = e->nextInBatch;
    }
    return sum;
}

static int qsg_countNodesInBatches(const QDataBuffer<Batch *> &batches)
{
    int sum = 0;
    for (int i=0; i<batches.size(); ++i) {
        sum += qsg_countNodesInBatch(batches.at(i));
    }
    return sum;
}

Renderer::Renderer(QSGRenderContext *ctx)
    : QSGRenderer(ctx)
    , m_opaqueRenderList(64)
    , m_alphaRenderList(64)
    , m_nextRenderOrder(0)
    , m_partialRebuild(false)
    , m_partialRebuildRoot(0)
    , m_opaqueBatches(16)
    , m_alphaBatches(16)
    , m_batchPool(16)
    , m_elementsToDelete(64)
    , m_tmpAlphaElements(16)
    , m_tmpOpaqueElements(16)
    , m_rebuild(FullRebuild)
    , m_zRange(0)
    , m_currentMaterial(0)
    , m_currentShader(0)
    , m_vao(0)
{
    setNodeUpdater(new Updater(this));

    m_shaderManager = ctx->findChild<ShaderManager *>(QStringLiteral("__qt_ShaderManager"), Qt::FindDirectChildrenOnly);
    if (!m_shaderManager) {
        m_shaderManager = new ShaderManager();
        m_shaderManager->setObjectName(QStringLiteral("__qt_ShaderManager"));
        m_shaderManager->setParent(ctx);
        QObject::connect(ctx, SIGNAL(invalidated()), m_shaderManager, SLOT(invalidated()), Qt::DirectConnection);
    }

    m_bufferStrategy = GL_STATIC_DRAW;
    QByteArray strategy = qgetenv("QSG_RENDERER_BUFFER_STRATEGY");
    if (strategy == "dynamic") {
        m_bufferStrategy = GL_DYNAMIC_DRAW;
    } else if (strategy == "stream") {
        m_bufferStrategy = GL_STREAM_DRAW;
    }

    m_batchNodeThreshold = 64;
    QByteArray alternateThreshold = qgetenv("QSG_RENDERER_BATCH_NODE_THRESHOLD");
    if (alternateThreshold.length() > 0) {
        bool ok = false;
        int threshold = alternateThreshold.toInt(&ok);
        if (ok)
            m_batchNodeThreshold = threshold;
    }

    m_batchVertexThreshold = 1024;
    alternateThreshold = qgetenv("QSG_RENDERER_BATCH_VERTEX_THRESHOLD");
    if (alternateThreshold.length() > 0) {
        bool ok = false;
        int threshold = alternateThreshold.toInt(&ok);
        if (ok)
            m_batchVertexThreshold = threshold;
    }
    if (Q_UNLIKELY(debug_build || debug_render)) {
        qDebug() << "Batch thresholds: nodes:" << m_batchNodeThreshold << " vertices:" << m_batchVertexThreshold;
        qDebug() << "Using buffer strategy:" << (m_bufferStrategy == GL_STATIC_DRAW ? "static" : (m_bufferStrategy == GL_DYNAMIC_DRAW ? "dynamic" : "stream"));
    }

    // If rendering with an OpenGL Core profile context, we need to create a VAO
    // to hold our vertex specification state.
    if (context()->openglContext()->format().profile() == QSurfaceFormat::CoreProfile) {
        m_vao = new QOpenGLVertexArrayObject(this);
        m_vao->create();
    }
}

static void qsg_wipeBuffer(Buffer *buffer, QOpenGLFunctions *funcs)
{
    funcs->glDeleteBuffers(1, &buffer->id);
    free(buffer->data);
}

static void qsg_wipeBatch(Batch *batch, QOpenGLFunctions *funcs)
{
    qsg_wipeBuffer(&batch->vbo, funcs);
    delete batch;
}

Renderer::~Renderer()
{
    // Clean up batches and buffers
    for (int i=0; i<m_opaqueBatches.size(); ++i) qsg_wipeBatch(m_opaqueBatches.at(i), this);
    for (int i=0; i<m_alphaBatches.size(); ++i) qsg_wipeBatch(m_alphaBatches.at(i), this);
    for (int i=0; i<m_batchPool.size(); ++i) qsg_wipeBatch(m_batchPool.at(i), this);

    // The shadowtree
    qDeleteAll(m_nodes.values());

    // Remaining elements...
    for (int i=0; i<m_elementsToDelete.size(); ++i) {
        Element *e = m_elementsToDelete.at(i);
        if (e->isRenderNode)
            delete static_cast<RenderNodeElement *>(e);
        else
            delete e;
    }
}

void Renderer::invalidateAndRecycleBatch(Batch *b)
{
    b->invalidate();
    for (int i=0; i<m_batchPool.size(); ++i)
        if (b == m_batchPool.at(i))
            return;
    m_batchPool.add(b);
}

/* The code here does a CPU-side allocation which might seem like a performance issue
 * compared to using glMapBuffer or glMapBufferRange which would give me back
 * potentially GPU allocated memory and saving me one deep-copy, but...
 *
 * Because we do a lot of CPU-side transformations, we need random-access memory
 * and the memory returned from glMapBuffer/glMapBufferRange is typically
 * uncached and thus very slow for our purposes.
 *
 * ref: http://www.opengl.org/wiki/Buffer_Object
 */
void Renderer::map(Buffer *buffer, int byteSize)
{
    if (buffer->size != byteSize) {
        if (buffer->data)
            free(buffer->data);
        buffer->data = (char *) malloc(byteSize);
        buffer->size = byteSize;
    }
}

void Renderer::unmap(Buffer *buffer)
{
    if (buffer->id == 0)
        glGenBuffers(1, &buffer->id);
    glBindBuffer(GL_ARRAY_BUFFER, buffer->id);
    glBufferData(GL_ARRAY_BUFFER, buffer->size, buffer->data, m_bufferStrategy);
}

BatchRootInfo *Renderer::batchRootInfo(Node *node)
{
    BatchRootInfo *info = node->rootInfo();
    if (!info) {
        if (node->type() == QSGNode::ClipNodeType)
            info = new ClipBatchRootInfo;
        else {
            Q_ASSERT(node->type() == QSGNode::TransformNodeType);
            info = new BatchRootInfo;
        }
        node->data = info;
    }
    return info;
}

void Renderer::removeBatchRootFromParent(Node *childRoot)
{
    BatchRootInfo *childInfo = batchRootInfo(childRoot);
    if (!childInfo->parentRoot)
        return;
    BatchRootInfo *parentInfo = batchRootInfo(childInfo->parentRoot);

    Q_ASSERT(parentInfo->subRoots.contains(childRoot));
    parentInfo->subRoots.remove(childRoot);
    childInfo->parentRoot = 0;
}

void Renderer::registerBatchRoot(Node *subRoot, Node *parentRoot)
{
    BatchRootInfo *subInfo = batchRootInfo(subRoot);
    BatchRootInfo *parentInfo = batchRootInfo(parentRoot);
    subInfo->parentRoot = parentRoot;
    parentInfo->subRoots << subRoot;
}

bool Renderer::changeBatchRoot(Node *node, Node *root)
{
    BatchRootInfo *subInfo = batchRootInfo(node);
    if (subInfo->parentRoot == root)
        return false;
    if (subInfo->parentRoot) {
        BatchRootInfo *oldRootInfo = batchRootInfo(subInfo->parentRoot);
        oldRootInfo->subRoots.remove(node);
    }
    BatchRootInfo *newRootInfo = batchRootInfo(root);
    newRootInfo->subRoots << node;
    subInfo->parentRoot = root;
    return true;
}

void Renderer::nodeChangedBatchRoot(Node *node, Node *root)
{
    if (node->type() == QSGNode::ClipNodeType || node->isBatchRoot) {
        if (!changeBatchRoot(node, root))
            return;
        node = root;
    } else if (node->type() == QSGNode::GeometryNodeType) {
        // Only need to change the root as nodeChanged anyway flags a full update.
        Element *e = node->element();
        if (e) {
            e->root = root;
            e->boundsComputed = false;
        }
    }

    SHADOWNODE_TRAVERSE(node)
            nodeChangedBatchRoot(*child, root);
}

void Renderer::nodeWasTransformed(Node *node, int *vertexCount)
{
    if (node->type() == QSGNode::GeometryNodeType) {
        QSGGeometryNode *gn = static_cast<QSGGeometryNode *>(node->sgNode);
        *vertexCount += gn->geometry()->vertexCount();
        Element *e  = node->element();
        if (e) {
            e->boundsComputed = false;
            if (e->batch) {
                if (!e->batch->isOpaque) {
                    if (e->root) {
                        m_taggedRoots << e->root;
                        m_rebuild |= BuildRenderListsForTaggedRoots;
                    } else {
                        m_rebuild |= FullRebuild;
                    }
                } else if (e->batch->merged) {
                    e->batch->needsUpload = true;
                }
            }
        }
    }

    SHADOWNODE_TRAVERSE(node)
        nodeWasTransformed(*child, vertexCount);
}

void Renderer::nodeWasAdded(QSGNode *node, Node *shadowParent)
{
    Q_ASSERT(!m_nodes.contains(node));
    if (node->isSubtreeBlocked())
        return;

    Node *snode = new Node(node);
    m_nodes.insert(node, snode);
    if (shadowParent) {
        snode->parent = shadowParent;
        shadowParent->children.append(snode);
    }

    if (node->type() == QSGNode::GeometryNodeType) {
        snode->data = new Element(static_cast<QSGGeometryNode *>(node));

    } else if (node->type() == QSGNode::ClipNodeType) {
        snode->data = new ClipBatchRootInfo;
        m_rebuild |= FullRebuild;

    } else if (node->type() == QSGNode::RenderNodeType) {
        RenderNodeElement *e = new RenderNodeElement(static_cast<QSGRenderNode *>(node));
        snode->data = e;
        Q_ASSERT(!m_renderNodeElements.contains(static_cast<QSGRenderNode *>(node)));
        m_renderNodeElements.insert(e->renderNode, e);
    }

    QSGNODE_TRAVERSE(node)
            nodeWasAdded(child, snode);
}

void Renderer::nodeWasRemoved(Node *node)
{
    // Prefix traversal as removeBatchFromParent below removes nodes
    // in a bottom-up manner
    SHADOWNODE_TRAVERSE(node)
            nodeWasRemoved(*child);

    if (node->type() == QSGNode::GeometryNodeType) {
        Element *e = node->element();
        if (e) {
            e->removed = true;
            m_elementsToDelete.add(e);
            e->node = 0;
            if (e->root) {
                BatchRootInfo *info = batchRootInfo(e->root);
                info->availableOrders++;
            }
            if (e->batch) {
                e->batch->needsUpload = true;
            }

        }

    } else if (node->type() == QSGNode::ClipNodeType) {
        removeBatchRootFromParent(node);
        delete node->clipInfo();
        m_rebuild |= FullRebuild;
        m_taggedRoots.remove(node);

    } else if (node->isBatchRoot) {
        removeBatchRootFromParent(node);
        delete node->rootInfo();
        m_rebuild |= FullRebuild;
        m_taggedRoots.remove(node);

    } else if (node->type() == QSGNode::RenderNodeType) {
        RenderNodeElement *e = m_renderNodeElements.take(static_cast<QSGRenderNode *>(node->sgNode));
        if (e) {
            e->removed = true;
            m_elementsToDelete.add(e);
        }
    }

    Q_ASSERT(m_nodes.contains(node->sgNode));
    delete m_nodes.take(node->sgNode);
}

void Renderer::turnNodeIntoBatchRoot(Node *node)
{
    if (Q_UNLIKELY(debug_change)) qDebug() << " - new batch root";
    m_rebuild |= FullRebuild;
    node->isBatchRoot = true;
    node->becameBatchRoot = true;

    Node *p = node->parent;
    while (p) {
        if (p->type() == QSGNode::ClipNodeType || p->isBatchRoot) {
            registerBatchRoot(node, p);
            break;
        }
        p = p->parent;
    }

    SHADOWNODE_TRAVERSE(node)
            nodeChangedBatchRoot(*child, node);
}


void Renderer::nodeChanged(QSGNode *node, QSGNode::DirtyState state)
{
    if (Q_UNLIKELY(debug_change)) {
        QDebug debug = qDebug();
        debug << "dirty:";
        if (state & QSGNode::DirtyGeometry)
            debug << "Geometry";
        if (state & QSGNode::DirtyMaterial)
            debug << "Material";
        if (state & QSGNode::DirtyMatrix)
            debug << "Matrix";
        if (state & QSGNode::DirtyNodeAdded)
            debug << "Added";
        if (state & QSGNode::DirtyNodeRemoved)
            debug << "Removed";
        if (state & QSGNode::DirtyOpacity)
            debug << "Opacity";
        if (state & QSGNode::DirtySubtreeBlocked)
            debug << "SubtreeBlocked";
        if (state & QSGNode::DirtyForceUpdate)
            debug << "ForceUpdate";

        // when removed, some parts of the node could already have been destroyed
        // so don't debug it out.
        if (state & QSGNode::DirtyNodeRemoved)
            debug << (void *) node << node->type();
        else
            debug << node;
    }

    // As this function calls nodeChanged recursively, we do it at the top
    // to avoid that any of the others are processed twice.
    if (state & QSGNode::DirtySubtreeBlocked) {
        Node *sn = m_nodes.value(node);
        bool blocked = node->isSubtreeBlocked();
        if (blocked && sn) {
            nodeChanged(node, QSGNode::DirtyNodeRemoved);
            Q_ASSERT(m_nodes.value(node) == 0);
        } else if (!blocked && !sn) {
            nodeChanged(node, QSGNode::DirtyNodeAdded);
        }
        return;
    }

    if (state & QSGNode::DirtyNodeAdded) {
        if (nodeUpdater()->isNodeBlocked(node, rootNode())) {
            QSGRenderer::nodeChanged(node, state);
            return;
        }
        if (node == rootNode())
            nodeWasAdded(node, 0);
        else
            nodeWasAdded(node, m_nodes.value(node->parent()));
    }

    // Mark this node dirty in the shadow tree.
    Node *shadowNode = m_nodes.value(node);

    // Blocked subtrees won't have shadow nodes, so we can safely abort
    // here..
    if (!shadowNode) {
        QSGRenderer::nodeChanged(node, state);
        return;
    }

    shadowNode->dirtyState |= state;

    if (state & QSGNode::DirtyMatrix && !shadowNode->isBatchRoot) {
        Q_ASSERT(node->type() == QSGNode::TransformNodeType);
        if (node->m_subtreeRenderableCount > m_batchNodeThreshold) {
            turnNodeIntoBatchRoot(shadowNode);
        } else {
            int vertices = 0;
            nodeWasTransformed(shadowNode, &vertices);
            if (vertices > m_batchVertexThreshold) {
                turnNodeIntoBatchRoot(shadowNode);
            }
        }
    }

    if (state & QSGNode::DirtyGeometry && node->type() == QSGNode::GeometryNodeType) {
        QSGGeometryNode *gn = static_cast<QSGGeometryNode *>(node);
        Element *e = shadowNode->element();
        if (e) {
            e->boundsComputed = false;
            Batch *b = e->batch;
            if (b) {
                if (!e->batch->geometryWasChanged(gn)) {
                    m_rebuild |= Renderer::FullRebuild;
                } else if (!b->isOpaque) {
                    if (e->root) {
                        m_taggedRoots << e->root;
                        m_rebuild |= BuildRenderListsForTaggedRoots;
                    } else {
                        m_rebuild |= FullRebuild;
                    }
                } else {
                    b->needsUpload = true;
                }
            }
        }
    }

    if (state & QSGNode::DirtyMaterial && node->type() == QSGNode::GeometryNodeType) {
        Element *e = shadowNode->element();
        if (e) {
            if (e->batch) {
                if (!e->batch->isMaterialCompatible(e))
                    m_rebuild = Renderer::FullRebuild;
            } else {
                m_rebuild |= Renderer::BuildBatches;
            }
        }
    }

    // Mark the shadow tree dirty all the way back to the root...
    QSGNode::DirtyState dirtyChain = state & (QSGNode::DirtyNodeAdded
                                              | QSGNode::DirtyOpacity
                                              | QSGNode::DirtyMatrix
                                              | QSGNode::DirtySubtreeBlocked
                                              | QSGNode::DirtyForceUpdate);
    if (dirtyChain != 0) {
        dirtyChain = QSGNode::DirtyState(dirtyChain << 16);
        Node *sn = shadowNode->parent;
        while (sn) {
            sn->dirtyState |= dirtyChain;
            sn = sn->parent;
        }
    }

    // Delete happens at the very end because it deletes the shadownode.
    if (state & QSGNode::DirtyNodeRemoved) {
        Node *parent = shadowNode->parent;
        if (parent)
            parent->children.removeOne(shadowNode);
        nodeWasRemoved(shadowNode);
        Q_ASSERT(m_nodes.value(node) == 0);
    }

    QSGRenderer::nodeChanged(node, state);
}

/*
 * Traverses the tree and builds two list of geometry nodes. One for
 * the opaque and one for the translucent. These are populated
 * in the order they should visually appear in, meaning first
 * to the back and last to the front.
 *
 * We split opaque and translucent as we can perform different
 * types of reordering / batching strategies on them, depending
 *
 * Note: It would be tempting to use the shadow nodes instead of the QSGNodes
 * for traversal to avoid hash lookups, but the order of the children
 * is important and they are not preserved in the shadow tree, so we must
 * use the actual QSGNode tree.
 */
void Renderer::buildRenderLists(QSGNode *node)
{
    if (node->isSubtreeBlocked())
        return;

    Q_ASSERT(m_nodes.contains(node));
    Node *shadowNode = m_nodes.value(node);

    if (node->type() == QSGNode::GeometryNodeType) {
        QSGGeometryNode *gn = static_cast<QSGGeometryNode *>(node);

        Element *e = shadowNode->element();
        Q_ASSERT(e);

        bool opaque = gn->inheritedOpacity() > OPAQUE_LIMIT && !(gn->activeMaterial()->flags() & QSGMaterial::Blending);
        if (opaque)
            m_opaqueRenderList << e;
        else
            m_alphaRenderList << e;

        e->order = ++m_nextRenderOrder;
        // Used while rebuilding partial roots.
        if (m_partialRebuild)
            e->orphaned = false;

    } else if (node->type() == QSGNode::ClipNodeType || shadowNode->isBatchRoot) {
        Q_ASSERT(m_nodes.contains(node));
        BatchRootInfo *info = batchRootInfo(m_nodes.value(node));
        if (node == m_partialRebuildRoot) {
            m_nextRenderOrder = info->firstOrder;
            QSGNODE_TRAVERSE(node)
                    buildRenderLists(child);
            m_nextRenderOrder = info->lastOrder + 1;
        } else {
            int currentOrder = m_nextRenderOrder;
            QSGNODE_TRAVERSE(node)
                buildRenderLists(child);
            int padding = (m_nextRenderOrder - currentOrder) >> 2;
            info->firstOrder = currentOrder;
            info->availableOrders = padding;
            info->lastOrder = m_nextRenderOrder + padding;
            m_nextRenderOrder = info->lastOrder;
        }
        return;
    } else if (node->type() == QSGNode::RenderNodeType) {
        RenderNodeElement *e = shadowNode->renderNodeElement();
        m_alphaRenderList << e;
        e->order = ++m_nextRenderOrder;
        Q_ASSERT(e);
    }

    QSGNODE_TRAVERSE(node)
        buildRenderLists(child);
}

void Renderer::tagSubRoots(Node *node)
{
    BatchRootInfo *i = batchRootInfo(node);
    m_taggedRoots << node;
    for (QSet<Node *>::const_iterator it = i->subRoots.constBegin();
         it != i->subRoots.constEnd(); ++it) {
        tagSubRoots(*it);
    }
}

static void qsg_addOrphanedElements(QDataBuffer<Element *> &orphans, const QDataBuffer<Element *> &renderList)
{
    orphans.reset();
    for (int i=0; i<renderList.size(); ++i) {
        Element *e = renderList.at(i);
        if (e && !e->removed) {
            e->orphaned = true;
            orphans.add(e);
        }
    }
}

static void qsg_addBackOrphanedElements(QDataBuffer<Element *> &orphans, QDataBuffer<Element *> &renderList)
{
    for (int i=0; i<orphans.size(); ++i) {
        Element *e = orphans.at(i);
        if (e->orphaned)
            renderList.add(e);
    }
    orphans.reset();
}

/*
 * To rebuild the tagged roots, we start by putting all subroots of tagged
 * roots into the list of tagged roots. This is to make the rest of the
 * algorithm simpler.
 *
 * Second, we invalidate all batches which belong to tagged roots, which now
 * includes the entire subtree under a given root
 *
 * Then we call buildRenderLists for all tagged subroots which do not have
 * parents which are tagged, aka, we traverse only the topmosts roots.
 *
 * Then we sort the render lists based on their render order, to restore the
 * right order for rendering.
 */
void Renderer::buildRenderListsForTaggedRoots()
{
    // Flag any element that is currently in the render lists, but which
    // is not in a batch. This happens when we have a partial rebuild
    // in one sub tree while we have a BuildBatches change in another
    // isolated subtree. So that batch-building takes into account
    // these "orphaned" nodes, we flag them now. The ones under tagged
    // roots will be cleared again. The remaining ones are added into the
    // render lists so that they contain all visual nodes after the
    // function completes.
    qsg_addOrphanedElements(m_tmpOpaqueElements, m_opaqueRenderList);
    qsg_addOrphanedElements(m_tmpAlphaElements, m_alphaRenderList);

    // Take a copy now, as we will be adding to this while traversing..
    QSet<Node *> roots = m_taggedRoots;
    for (QSet<Node *>::const_iterator it = roots.constBegin();
         it != roots.constEnd(); ++it) {
        tagSubRoots(*it);
    }

    for (int i=0; i<m_opaqueBatches.size(); ++i) {
        Batch *b = m_opaqueBatches.at(i);
        if (m_taggedRoots.contains(b->root))
            invalidateAndRecycleBatch(b);

    }
    for (int i=0; i<m_alphaBatches.size(); ++i) {
        Batch *b = m_alphaBatches.at(i);
        if (m_taggedRoots.contains(b->root))
            invalidateAndRecycleBatch(b);
    }

    m_opaqueRenderList.reset();
    m_alphaRenderList.reset();
    int maxRenderOrder = m_nextRenderOrder;
    m_partialRebuild = true;
    // Traverse each root, assigning it
    for (QSet<Node *>::const_iterator it = m_taggedRoots.constBegin();
         it != m_taggedRoots.constEnd(); ++it) {
        Node *root = *it;
        BatchRootInfo *i = batchRootInfo(root);
        if ((!i->parentRoot || !m_taggedRoots.contains(i->parentRoot))
             && !nodeUpdater()->isNodeBlocked(root->sgNode, rootNode())) {
            m_nextRenderOrder = i->firstOrder;
            m_partialRebuildRoot = root->sgNode;
            buildRenderLists(root->sgNode);
        }
    }
    m_partialRebuild = false;
    m_partialRebuildRoot = 0;
    m_taggedRoots.clear();
    m_nextRenderOrder = qMax(m_nextRenderOrder, maxRenderOrder);

    // Add orphaned elements back into the list and then sort it..
    qsg_addBackOrphanedElements(m_tmpOpaqueElements, m_opaqueRenderList);
    qsg_addBackOrphanedElements(m_tmpAlphaElements, m_alphaRenderList);

    if (m_opaqueRenderList.size())
        std::sort(&m_opaqueRenderList.first(), &m_opaqueRenderList.last() + 1, qsg_sort_element_decreasing_order);
    if (m_alphaRenderList.size())
        std::sort(&m_alphaRenderList.first(), &m_alphaRenderList.last() + 1, qsg_sort_element_increasing_order);

}

void Renderer::buildRenderListsFromScratch()
{
    m_opaqueRenderList.reset();
    m_alphaRenderList.reset();

    for (int i=0; i<m_opaqueBatches.size(); ++i)
        invalidateAndRecycleBatch(m_opaqueBatches.at(i));
    for (int i=0; i<m_alphaBatches.size(); ++i)
        invalidateAndRecycleBatch(m_alphaBatches.at(i));
    m_opaqueBatches.reset();
    m_alphaBatches.reset();

    m_nextRenderOrder = 0;

    buildRenderLists(rootNode());
}

void Renderer::invalidateAlphaBatchesForRoot(Node *root)
{
    for (int i=0; i<m_alphaBatches.size(); ++i) {
        Batch *b = m_alphaBatches.at(i);
        if (b->root == root || root == 0)
            b->invalidate();
    }
}

/* Clean up batches by making it a consecutive list of "valid"
 * batches and moving all invalidated batches to the batches pool.
 */
void Renderer::cleanupBatches(QDataBuffer<Batch *> *batches) {
    if (batches->size()) {
        std::sort(&batches->first(), &batches->last() + 1, qsg_sort_batch_is_valid);
        int count = 0;
        while (count < batches->size() && batches->at(count)->first)
            ++count;
        for (int i=count; i<batches->size(); ++i)
            invalidateAndRecycleBatch(batches->at(i));
        batches->resize(count);
    }
}

void Renderer::prepareOpaqueBatches()
{
    for (int i=m_opaqueRenderList.size() - 1; i >= 0; --i) {
        Element *ei = m_opaqueRenderList.at(i);
        if (!ei || ei->batch)
            continue;
        Batch *batch = newBatch();
        batch->first = ei;
        batch->root = ei->root;
        batch->isOpaque = true;
        batch->needsUpload = true;
        batch->positionAttribute = qsg_positionAttribute(ei->node->geometry());

        m_opaqueBatches.add(batch);

        ei->batch = batch;
        Element *next = ei;

        QSGGeometryNode *gni = ei->node;

        for (int j = i - 1; j >= 0; --j) {
            Element *ej = m_opaqueRenderList.at(j);
            if (!ej)
                continue;
            if (ej->root != ei->root)
                break;
            if (ej->batch)
                continue;

            QSGGeometryNode *gnj = ej->node;

            if (gni->clipList() == gnj->clipList()
                    && gni->geometry()->drawingMode() == gnj->geometry()->drawingMode()
                    && gni->geometry()->attributes() == gnj->geometry()->attributes()
                    && gni->inheritedOpacity() == gnj->inheritedOpacity()
                    && gni->activeMaterial()->type() == gnj->activeMaterial()->type()
                    && gni->activeMaterial()->compare(gnj->activeMaterial()) == 0) {
                ej->batch = batch;
                next->nextInBatch = ej;
                next = ej;
            }
        }
    }
}

bool Renderer::checkOverlap(int first, int last, const Rect &bounds)
{
    for (int i=first; i<=last; ++i) {
        Element *e = m_alphaRenderList.at(i);
        if (!e || e->batch)
            continue;
        Q_ASSERT(e->boundsComputed);
        if (e->bounds.intersects(bounds))
            return true;
    }
    return false;
}

/*
 *
 * To avoid the O(n^2) checkOverlap check in most cases, we have the
 * overlapBounds which is the union of all bounding rects to check overlap
 * for. We know that if it does not overlap, then none of the individual
 * ones will either. For the typical list case, this results in no calls
 * to checkOverlap what-so-ever. This also ensures that when all consecutive
 * items are matching (such as a table of text), we don't build up an
 * overlap bounds and thus do not require full overlap checks.
 */

void Renderer::prepareAlphaBatches()
{
    for (int i=0; i<m_alphaRenderList.size(); ++i) {
        Element *e = m_alphaRenderList.at(i);
        if (!e || e->isRenderNode)
            continue;
        Q_ASSERT(!e->removed);
        e->ensureBoundsValid();
    }

    for (int i=0; i<m_alphaRenderList.size(); ++i) {
        Element *ei = m_alphaRenderList.at(i);
        if (!ei || ei->batch)
            continue;

        if (ei->isRenderNode) {
            Batch *rnb = newBatch();
            rnb->first = ei;
            rnb->root = ei->root;
            rnb->isOpaque = false;
            rnb->isRenderNode = true;
            ei->batch = rnb;
            m_alphaBatches.add(rnb);
            continue;
        }

        Batch *batch = newBatch();
        batch->first = ei;
        batch->root = ei->root;
        batch->isOpaque = false;
        batch->needsUpload = true;
        m_alphaBatches.add(batch);
        ei->batch = batch;

        QSGGeometryNode *gni = ei->node;
        batch->positionAttribute = qsg_positionAttribute(gni->geometry());

        Rect overlapBounds;
        overlapBounds.set(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);

        Element *next = ei;

        for (int j = i + 1; j < m_alphaRenderList.size(); ++j) {
            Element *ej = m_alphaRenderList.at(j);
            if (!ej)
                continue;
            if (ej->root != ei->root || ej->isRenderNode)
                break;
            if (ej->batch)
                continue;

            QSGGeometryNode *gnj = ej->node;

            if (gni->clipList() == gnj->clipList()
                    && gni->geometry()->drawingMode() == gnj->geometry()->drawingMode()
                    && gni->geometry()->attributes() == gnj->geometry()->attributes()
                    && gni->inheritedOpacity() == gnj->inheritedOpacity()
                    && gni->activeMaterial()->type() == gnj->activeMaterial()->type()
                    && gni->activeMaterial()->compare(gnj->activeMaterial()) == 0) {
                if (!overlapBounds.intersects(ej->bounds) || !checkOverlap(i+1, j - 1, ej->bounds)) {
                    ej->batch = batch;
                    next->nextInBatch = ej;
                    next = ej;
                } else {
                    /* When we come across a compatible element which hits an overlap, we
                     * need to stop the batch right away. We cannot add more elements
                     * to the current batch as they will be rendered before the batch that the
                     * current 'ej' will be added to.
                     */
                    break;
                }
            } else {
                overlapBounds |= ej->bounds;
            }
        }
    }


}

/* These parameters warrant some explanation...
 *
 * vaOffset: The byte offset into the vertex data to the location of the
 *           2D float point vertex attributes.
 *
 * vertexData: destination where the geometry's vertex data should go
 *
 * zData: destination of geometries injected Z positioning
 *
 * indexData: destination of the indices for this element
 *
 * iBase: The starting index for this element in the batch
 */

void Renderer::uploadMergedElement(Element *e, int vaOffset, char **vertexData, char **zData, char **indexData, quint16 *iBase, int *indexCount)
{
    if (Q_UNLIKELY(debug_upload)) qDebug() << "  - uploading element:" << e << e->node << (void *) *vertexData << (qintptr) (*zData - *vertexData) << (qintptr) (*indexData - *vertexData);
    QSGGeometry *g = e->node->geometry();

    const QMatrix4x4 &localx = *e->node->matrix();

    const int vCount = g->vertexCount();
    const int vSize = g->sizeOfVertex();
    memcpy(*vertexData, g->vertexData(), vSize * vCount);

    // apply vertex transform..
    char *vdata = *vertexData + vaOffset;
    if (((const QMatrix4x4_Accessor &) localx).flagBits == 1) {
        for (int i=0; i<vCount; ++i) {
            Pt *p = (Pt *) vdata;
            p->x += ((QMatrix4x4_Accessor &) localx).m[3][0];
            p->y += ((QMatrix4x4_Accessor &) localx).m[3][1];
            vdata += vSize;
        }
    } else if (((const QMatrix4x4_Accessor &) localx).flagBits > 1) {
        for (int i=0; i<vCount; ++i) {
            ((Pt *) vdata)->map(localx);
            vdata += vSize;
        }
    }

    float *vzorder = (float *) *zData;
    float zorder = 1.0f - e->order * m_zRange;
    for (int i=0; i<vCount; ++i)
        vzorder[i] = zorder;

    int iCount = g->indexCount();
    quint16 *indices = (quint16 *) *indexData;

    if (iCount == 0) {
        if (g->drawingMode() == GL_TRIANGLE_STRIP)
            *indices++ = *iBase;
        iCount = vCount;
        for (int i=0; i<iCount; ++i)
            indices[i] = *iBase + i;
    } else {
        const quint16 *srcIndices = g->indexDataAsUShort();
        if (g->drawingMode() == GL_TRIANGLE_STRIP)
            *indices++ = *iBase + srcIndices[0];
        for (int i=0; i<iCount; ++i)
            indices[i] = *iBase + srcIndices[i];
    }
    if (g->drawingMode() == GL_TRIANGLE_STRIP) {
        indices[iCount] = indices[iCount - 1];
        iCount += 2;
    }

    *vertexData += vCount * vSize;
    *zData += vCount * sizeof(float);
    *indexData += iCount * sizeof(quint16);
    *iBase += vCount;
    *indexCount += iCount;
}

const QMatrix4x4 &Renderer::matrixForRoot(Node *node)
{
    if (node->type() == QSGNode::TransformNodeType)
        return static_cast<QSGTransformNode *>(node->sgNode)->combinedMatrix();
    Q_ASSERT(node->type() == QSGNode::ClipNodeType);
    QSGClipNode *c = static_cast<QSGClipNode *>(node->sgNode);
    return *c->matrix();
}

void Renderer::uploadBatch(Batch *b)
{
        // Early out if nothing has changed in this batch..
        if (!b->needsUpload) {
            if (Q_UNLIKELY(debug_upload)) qDebug() << " Batch:" << b << "already uploaded...";
            return;
        }

        if (!b->first) {
            if (Q_UNLIKELY(debug_upload)) qDebug() << " Batch:" << b << "is invalid...";
            return;
        }

        if (b->isRenderNode) {
            if (Q_UNLIKELY(debug_upload)) qDebug() << " Batch: " << b << "is a render node...";
            return;
        }

        // Figure out if we can merge or not, if not, then just render the batch as is..
        Q_ASSERT(b->first);
        Q_ASSERT(b->first->node);

        QSGGeometryNode *gn = b->first->node;
        QSGGeometry *g =  gn->geometry();

        bool canMerge = (g->drawingMode() == GL_TRIANGLES || g->drawingMode() == GL_TRIANGLE_STRIP)
                        && b->positionAttribute >= 0
                        && g->indexType() == GL_UNSIGNED_SHORT
                        && (gn->activeMaterial()->flags() & QSGMaterial::CustomCompileStep) == 0
                        && (((gn->activeMaterial()->flags() & QSGMaterial::RequiresDeterminant) == 0)
                            || (((gn->activeMaterial()->flags() & QSGMaterial_RequiresFullMatrixBit) == 0) && b->isTranslateOnlyToRoot())
                            )
                        && b->isSafeToBatch();

        b->merged = canMerge;

        // Figure out how much memory we need...
        b->vertexCount = 0;
        b->indexCount = 0;
        int unmergedIndexSize = 0;
        Element *e = b->first;

        while (e) {
            QSGGeometry *eg = e->node->geometry();
            b->vertexCount += eg->vertexCount();
            int iCount = eg->indexCount();
            if (b->merged) {
                if (iCount == 0)
                    iCount = eg->vertexCount();
                // merged Triangle strips need to contain degenerate triangles at the beginning and end.
                // One could save 2 ushorts here by ditching the the padding for the front of the
                // first and the end of the last, but for simplicity, we simply don't care.
                if (g->drawingMode() == GL_TRIANGLE_STRIP)
                    iCount += sizeof(quint16);
            } else {
                unmergedIndexSize += iCount * eg->sizeOfIndex();
            }
            b->indexCount += iCount;
            e = e->nextInBatch;
        }

        // Abort if there are no vertices in this batch.. We abort this late as
        // this is a broken usecase which we do not care to optimize for...
        if (b->vertexCount == 0 || (b->merged && b->indexCount == 0))
            return;

        /* Allocate memory for this batch. Merged batches are divided into three separate blocks
           1. Vertex data for all elements, as they were in the QSGGeometry object, but
              with the tranform relative to this batch's root applied. The vertex data
              is otherwise unmodified.
           2. Z data for all elements, derived from each elements "render order".
              This is present for merged data only.
           3. Indices for all elements, as they were in the QSGGeometry object, but
              adjusted so that each index matches its.
              And for TRIANGLE_STRIPs, we need to insert degenerate between each
              primitive. These are unsigned shorts for merged and arbitrary for
              non-merged.
         */
        int bufferSize =  b->vertexCount * g->sizeOfVertex();
        if (b->merged)
            bufferSize += b->vertexCount * sizeof(float) + b->indexCount * sizeof(quint16);
        else
            bufferSize += unmergedIndexSize;
        map(&b->vbo, bufferSize);

        if (Q_UNLIKELY(debug_upload)) qDebug() << " - batch" << b << " first:" << b->first << " root:"
                                   << b->root << " merged:" << b->merged << " positionAttribute" << b->positionAttribute
                                   << " vbo:" << b->vbo.id << ":" << b->vbo.size;

        if (b->merged) {
            char *vertexData = b->vbo.data;
            char *zData = vertexData + b->vertexCount * g->sizeOfVertex();
            char *indexData = zData + b->vertexCount * sizeof(float);

            quint16 iOffset = 0;
            e = b->first;
            int verticesInSet = 0;
            int indicesInSet = 0;
            b->drawSets.reset();
            b->drawSets << DrawSet(0, zData - vertexData, indexData - vertexData);
            while (e) {
                verticesInSet  += e->node->geometry()->vertexCount();
                if (verticesInSet > 0xffff) {
                    b->drawSets.last().indexCount = indicesInSet;
                    b->drawSets << DrawSet(vertexData - b->vbo.data,
                                           zData - b->vbo.data,
                                           indexData - b->vbo.data);
                    iOffset = 0;
                    verticesInSet = e->node->geometry()->vertexCount();
                    indicesInSet = 0;
                }
                uploadMergedElement(e, b->positionAttribute, &vertexData, &zData, &indexData, &iOffset, &indicesInSet);
                e = e->nextInBatch;
            }
            b->drawSets.last().indexCount = indicesInSet;
        } else {
            char *vboData = b->vbo.data;
            char *iboData = vboData + b->vertexCount * g->sizeOfVertex();
            Element *e = b->first;
            while (e) {
                QSGGeometry *g = e->node->geometry();
                int vbs = g->vertexCount() * g->sizeOfVertex();
                memcpy(vboData, g->vertexData(), vbs);
                vboData = vboData + vbs;
                if (g->indexCount()) {
                    int ibs = g->indexCount() * g->sizeOfIndex();
                    memcpy(iboData, g->indexData(), ibs);
                    iboData += ibs;
                }
                e = e->nextInBatch;
            }
        }

        if (Q_UNLIKELY(debug_upload)) {
            const char *vd = b->vbo.data;
            qDebug() << "  -- Vertex Data, count:" << b->vertexCount << " - " << g->sizeOfVertex() << "bytes/vertex";
            for (int i=0; i<b->vertexCount; ++i) {
                QDebug dump = qDebug().nospace();
                dump << "  --- " << i << ": ";
                int offset = 0;
                for (int a=0; a<g->attributeCount(); ++a) {
                    const QSGGeometry::Attribute &attr = g->attributes()[a];
                    dump << attr.position << ":(" << attr.tupleSize << ",";
                    if (attr.type == GL_FLOAT) {
                        dump << "float ";
                        if (attr.isVertexCoordinate)
                            dump << "* ";
                        for (int t=0; t<attr.tupleSize; ++t)
                            dump << *(float *)(vd + offset + t * sizeof(float)) << " ";
                    } else if (attr.type == GL_UNSIGNED_BYTE) {
                        dump << "ubyte ";
                        for (int t=0; t<attr.tupleSize; ++t)
                            dump << *(unsigned char *)(vd + offset + t * sizeof(unsigned char)) << " ";
                    }
                    dump << ") ";
                    offset += attr.tupleSize * size_of_type(attr.type);
                }
                if (b->merged) {
                    float zorder = ((float*)(b->vbo.data + b->vertexCount * g->sizeOfVertex()))[i];
                    dump << " Z:(" << zorder << ")";
                }
                vd += g->sizeOfVertex();
            }

            const quint16 *id = (const quint16 *) (b->vbo.data
                                                   + b->vertexCount * g->sizeOfVertex()
                                                   + (b->merged ? b->vertexCount * sizeof(float) : 0));
            {
                QDebug iDump = qDebug();
                iDump << "  -- Index Data, count:" << b->indexCount;
                for (int i=0; i<b->indexCount; ++i) {
                    if ((i % 24) == 0)
                       iDump << endl << "  --- ";
                 iDump << id[i];
                }
            }

            for (int i=0; i<b->drawSets.size(); ++i) {
                const DrawSet &s = b->drawSets.at(i);
                qDebug() << "  -- DrawSet: indexCount:" << s.indexCount << " vertices:" << s.vertices << " z:" << s.zorders << " indices:" << s.indices;
            }
        }

        unmap(&b->vbo);

        if (Q_UNLIKELY(debug_upload)) qDebug() << "  --- vertex/index buffers unmapped, batch upload completed...";

        b->needsUpload = false;

        if (Q_UNLIKELY(debug_render))
            b->uploadedThisFrame = true;
}

void Renderer::updateClip(const QSGClipNode *clipList, const Batch *batch)
{
    if (clipList != m_currentClip && Q_LIKELY(!debug_noclip)) {
        m_currentClip = clipList;
        // updateClip sets another program, so force-reactivate our own
        if (m_currentShader)
            setActiveShader(0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        if (batch->isOpaque)
            glDisable(GL_DEPTH_TEST);
        ClipType type = updateStencilClip(m_currentClip);
        if (batch->isOpaque) {
            glEnable(GL_DEPTH_TEST);
            if (type & StencilClip)
                glDepthMask(true);
        }
    }
}

/*!
 * Look at the attribute arrays and potentially the injected z attribute to figure out
 * which vertex attribute arrays need to be enabled and not. Then update the current
 * Shader and current QSGMaterialShader.
 */
void Renderer::setActiveShader(QSGMaterialShader *program, ShaderManager::Shader *shader)
{
    const char * const *c = m_currentProgram ? m_currentProgram->attributeNames() : 0;
    const char * const *n = program ? program->attributeNames() : 0;

    int cza = m_currentShader ? m_currentShader->pos_order : -1;
    int nza = shader ? shader->pos_order : -1;

    int i = 0;
    while (c || n) {

        bool was = c;
        if (cza == i) {
            was = true;
            c = 0;
        } else if (c && !c[i]) { // end of the attribute array names
            c = 0;
            was = false;
        }

        bool is = n;
        if (nza == i) {
            is = true;
            n = 0;
        } else if (n && !n[i]) {
            n = 0;
            is = false;
        }

        if (is && !was)
            glEnableVertexAttribArray(i);
        else if (was && !is)
            glDisableVertexAttribArray(i);

        ++i;
    }

    if (m_currentProgram)
        m_currentProgram->deactivate();
    m_currentProgram = program;
    m_currentShader = shader;
    m_currentMaterial = 0;
    if (m_currentProgram) {
        m_currentProgram->program()->bind();
        m_currentProgram->activate();
    }
}

void Renderer::renderMergedBatch(const Batch *batch)
{
    if (batch->vertexCount == 0 || batch->indexCount == 0)
        return;

    Element *e = batch->first;
    Q_ASSERT(e);

    if (Q_UNLIKELY(debug_render)) {
        QDebug debug = qDebug();
        debug << " -"
              << batch
              << (batch->uploadedThisFrame ? "[  upload]" : "[retained]")
              << (e->node->clipList() ? "[  clip]" : "[noclip]")
              << (batch->isOpaque ? "[opaque]" : "[ alpha]")
              << "[  merged]"
              << " Nodes:" << QString::fromLatin1("%1").arg(qsg_countNodesInBatch(batch), 4).toLatin1().constData()
              << " Vertices:" << QString::fromLatin1("%1").arg(batch->vertexCount, 5).toLatin1().constData()
              << " Indices:" << QString::fromLatin1("%1").arg(batch->indexCount, 5).toLatin1().constData()
              << " root:" << batch->root;
        if (batch->drawSets.size() > 1)
            debug << "sets:" << batch->drawSets.size();
        batch->uploadedThisFrame = false;
    }

    QSGGeometryNode *gn = e->node;

    // We always have dirty matrix as all batches are at a unique z range.
    QSGMaterialShader::RenderState::DirtyStates dirty = QSGMaterialShader::RenderState::DirtyMatrix;
    if (batch->root)
        m_current_model_view_matrix = matrixForRoot(batch->root);
    else
        m_current_model_view_matrix.setToIdentity();
    m_current_determinant = m_current_model_view_matrix.determinant();
    m_current_projection_matrix = projectionMatrix(); // has potentially been changed by renderUnmergedBatch..

    // updateClip() uses m_current_projection_matrix.
    updateClip(gn->clipList(), batch);

    glBindBuffer(GL_ARRAY_BUFFER, batch->vbo.id);

    char *indexBase = 0;
    if (m_context->hasBrokenIndexBufferObjects()) {
        indexBase = batch->vbo.data;
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    } else {
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, batch->vbo.id);
    }


    QSGMaterial *material = gn->activeMaterial();
    ShaderManager::Shader *sms = m_shaderManager->prepareMaterial(material);
    QSGMaterialShader *program = sms->program;

    if (m_currentShader != sms)
        setActiveShader(program, sms);

    m_current_opacity = gn->inheritedOpacity();
    if (sms->lastOpacity != m_current_opacity) {
        dirty |= QSGMaterialShader::RenderState::DirtyOpacity;
        sms->lastOpacity = m_current_opacity;
    }

    program->updateState(state(dirty), material, m_currentMaterial);

    m_currentMaterial = material;

    QSGGeometry* g = gn->geometry();
    char const *const *attrNames = program->attributeNames();
    for (int i=0; i<batch->drawSets.size(); ++i) {
        const DrawSet &draw = batch->drawSets.at(i);
        int offset = 0;
        for (int j = 0; attrNames[j]; ++j) {
            if (!*attrNames[j])
                continue;
            const QSGGeometry::Attribute &a = g->attributes()[j];
            GLboolean normalize = a.type != GL_FLOAT && a.type != GL_DOUBLE;
            glVertexAttribPointer(a.position, a.tupleSize, a.type, normalize, g->sizeOfVertex(), (void *) (qintptr) (offset + draw.vertices));
            offset += a.tupleSize * size_of_type(a.type);
        }
        glVertexAttribPointer(sms->pos_order, 1, GL_FLOAT, false, 0, (void *) (qintptr) (draw.zorders));

        glDrawElements(g->drawingMode(), draw.indexCount, GL_UNSIGNED_SHORT, (void *) (qintptr) (indexBase + draw.indices));
    }
}

void Renderer::renderUnmergedBatch(const Batch *batch)
{
    if (batch->vertexCount == 0)
        return;

    Element *e = batch->first;
    Q_ASSERT(e);

    if (Q_UNLIKELY(debug_render)) {
        qDebug() << " -"
                 << batch
                 << (batch->uploadedThisFrame ? "[  upload]" : "[retained]")
                 << (e->node->clipList() ? "[  clip]" : "[noclip]")
                 << (batch->isOpaque ? "[opaque]" : "[ alpha]")
                 << "[unmerged]"
                 << " Nodes:" << QString::fromLatin1("%1").arg(qsg_countNodesInBatch(batch), 4).toLatin1().constData()
                 << " Vertices:" << QString::fromLatin1("%1").arg(batch->vertexCount, 5).toLatin1().constData()
                 << " Indices:" << QString::fromLatin1("%1").arg(batch->indexCount, 5).toLatin1().constData()
                 << " root:" << batch->root;

        batch->uploadedThisFrame = false;
    }

    QSGGeometryNode *gn = e->node;

    m_current_projection_matrix = projectionMatrix();
    updateClip(gn->clipList(), batch);

    glBindBuffer(GL_ARRAY_BUFFER, batch->vbo.id);
    char *indexBase = 0;
    if (batch->indexCount) {
        if (m_context->hasBrokenIndexBufferObjects()) {
            indexBase = batch->vbo.data;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        } else {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, batch->vbo.id);
        }
    }

    // We always have dirty matrix as all batches are at a unique z range.
    QSGMaterialShader::RenderState::DirtyStates dirty = QSGMaterialShader::RenderState::DirtyMatrix;

    QSGMaterial *material = gn->activeMaterial();
    ShaderManager::Shader *sms = m_shaderManager->prepareMaterialNoRewrite(material);
    QSGMaterialShader *program = sms->program;

    if (sms != m_currentShader)
        setActiveShader(program, sms);

    m_current_opacity = gn->inheritedOpacity();
    if (sms->lastOpacity != m_current_opacity) {
        dirty |= QSGMaterialShader::RenderState::DirtyOpacity;
        sms->lastOpacity = m_current_opacity;
    }

    int vOffset = 0;
    char *iOffset = indexBase + batch->vertexCount * gn->geometry()->sizeOfVertex();

    QMatrix4x4 rootMatrix = batch->root ? matrixForRoot(batch->root) : QMatrix4x4();

    while (e) {
        gn = e->node;

        m_current_model_view_matrix = rootMatrix * *gn->matrix();
        m_current_determinant = m_current_model_view_matrix.determinant();

        m_current_projection_matrix = projectionMatrix();
        m_current_projection_matrix(2, 2) = m_zRange;
        m_current_projection_matrix(2, 3) = 1.0f - e->order * m_zRange;

        program->updateState(state(dirty), material, m_currentMaterial);

        // We don't need to bother with asking each node for its material as they
        // are all identical (compare==0) since they are in the same batch.
        m_currentMaterial = material;

        QSGGeometry* g = gn->geometry();
        char const *const *attrNames = program->attributeNames();
        int offset = 0;
        for (int j = 0; attrNames[j]; ++j) {
            if (!*attrNames[j])
                continue;
            const QSGGeometry::Attribute &a = g->attributes()[j];
            GLboolean normalize = a.type != GL_FLOAT && a.type != GL_DOUBLE;
            glVertexAttribPointer(a.position, a.tupleSize, a.type, normalize, g->sizeOfVertex(), (void *) (qintptr) (offset + vOffset));
            offset += a.tupleSize * size_of_type(a.type);
        }

        if (g->drawingMode() == GL_LINE_STRIP || g->drawingMode() == GL_LINE_LOOP || g->drawingMode() == GL_LINES)
            glLineWidth(g->lineWidth());

        if (g->indexCount())
            glDrawElements(g->drawingMode(), g->indexCount(), g->indexType(), iOffset);
        else
            glDrawArrays(g->drawingMode(), 0, g->vertexCount());

        vOffset += g->sizeOfVertex() * g->vertexCount();
        iOffset += g->indexCount() * g->sizeOfIndex();

        // We only need to push this on the very first iteration...
        dirty &= ~QSGMaterialShader::RenderState::DirtyOpacity;

        e = e->nextInBatch;
    }
}

void Renderer::renderBatches()
{
    if (Q_UNLIKELY(debug_render)) {
        qDebug().nospace() << "Rendering:" << endl
                           << " -> Opaque: " << qsg_countNodesInBatches(m_opaqueBatches) << " nodes in " << m_opaqueBatches.size() << " batches..." << endl
                           << " -> Alpha: " << qsg_countNodesInBatches(m_alphaBatches) << " nodes in " << m_alphaBatches.size() << " batches...";
    }

    for (QHash<QSGRenderNode *, RenderNodeElement *>::const_iterator it = m_renderNodeElements.constBegin();
         it != m_renderNodeElements.constEnd(); ++it) {
        prepareRenderNode(it.value());
    }

    QRect r = viewportRect();
    glViewport(r.x(), deviceRect().bottom() - r.bottom(), r.width(), r.height());
    glClearColor(clearColor().redF(), clearColor().greenF(), clearColor().blueF(), clearColor().alphaF());
#if defined(QT_OPENGL_ES)
    glClearDepthf(1);
#else
    glClearDepth(1);
#endif

    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(true);
    glColorMask(true, true, true, true);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);

    bindable()->clear(clearMode());

    m_current_opacity = 1;
    m_currentMaterial = 0;
    m_currentShader = 0;
    m_currentProgram = 0;
    m_currentClip = 0;

    bool renderOpaque = !debug_noopaque;
    bool renderAlpha = !debug_noalpha;

    if (Q_LIKELY(renderOpaque)) {
        for (int i=0; i<m_opaqueBatches.size(); ++i) {
            Batch *b = m_opaqueBatches.at(i);
            if (b->merged)
                renderMergedBatch(b);
            else
                renderUnmergedBatch(b);
        }
    }

    glEnable(GL_BLEND);
    glDepthMask(false);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    if (Q_LIKELY(renderAlpha)) {
        for (int i=0; i<m_alphaBatches.size(); ++i) {
            Batch *b = m_alphaBatches.at(i);
            if (b->merged)
                renderMergedBatch(b);
            else if (b->isRenderNode)
                renderRenderNode(b);
            else
                renderUnmergedBatch(b);
        }
    }

    if (m_currentShader)
        setActiveShader(0, 0);
    updateStencilClip(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

}

void Renderer::deleteRemovedElements()
{
    if (!m_elementsToDelete.size())
        return;

    for (int i=0; i<m_opaqueRenderList.size(); ++i) {
        Element **e = m_opaqueRenderList.data() + i;
        if (*e && (*e)->removed)
            *e = 0;
    }
    for (int i=0; i<m_alphaRenderList.size(); ++i) {
        Element **e = m_alphaRenderList.data() + i;
        if (*e && (*e)->removed)
            *e = 0;
    }

    for (int i=0; i<m_elementsToDelete.size(); ++i) {
        Element *e = m_elementsToDelete.at(i);
        if (e->isRenderNode)
            delete static_cast<RenderNodeElement *>(e);
        else
            delete e;
    }
    m_elementsToDelete.reset();
}

void Renderer::preprocess()
{
    // Bind our VAO. It's important that we do this here as the
    // QSGRenderer::preprocess() call may well do work that requires
    // a bound VAO.
    if (m_vao)
        m_vao->bind();

    QSGRenderer::preprocess();
}

void Renderer::render()
{
    if (Q_UNLIKELY(debug_dump)) {
        qDebug("\n");
        QSGNodeDumper::dump(rootNode());
    }

    if (Q_UNLIKELY(debug_render || debug_build)) {

        QByteArray type("rebuild:");
        if (m_rebuild == 0)
            type += " none";
        if (m_rebuild == FullRebuild)
            type += " full";
        else {
            if (m_rebuild & BuildRenderLists)
                type += " renderlists";
            else if (m_rebuild & BuildRenderListsForTaggedRoots)
                type += " partial";
            else if (m_rebuild & BuildBatches)
                type += " batches";
        }


        qDebug() << "Renderer::render()" << this << type;
    }

    if (m_rebuild & (BuildRenderLists | BuildRenderListsForTaggedRoots)) {
        bool complete = (m_rebuild & BuildRenderLists) != 0;
        if (complete)
            buildRenderListsFromScratch();
        else
            buildRenderListsForTaggedRoots();
        m_rebuild |= BuildBatches;

        if (Q_UNLIKELY(debug_build)) {
            qDebug() << "Opaque render lists" << (complete ? "(complete)" : "(partial)") << ":";
            for (int i=0; i<m_opaqueRenderList.size(); ++i) {
                Element *e = m_opaqueRenderList.at(i);
                qDebug() << " - element:" << e << " batch:" << e->batch << " node:" << e->node << " order:" << e->order;
            }
            qDebug() << "Alpha render list:" << (complete ? "(complete)" : "(partial)") << ":";
            for (int i=0; i<m_alphaRenderList.size(); ++i) {
                Element *e = m_alphaRenderList.at(i);
                qDebug() << " - element:" << e << " batch:" << e->batch << " node:" << e->node << " order:" << e->order;
            }
        }
    }

    for (int i=0; i<m_opaqueBatches.size(); ++i)
        m_opaqueBatches.at(i)->cleanupRemovedElements();
    for (int i=0; i<m_alphaBatches.size(); ++i)
        m_alphaBatches.at(i)->cleanupRemovedElements();
    deleteRemovedElements();

    cleanupBatches(&m_opaqueBatches);
    cleanupBatches(&m_alphaBatches);


    if (m_rebuild & BuildBatches) {
        prepareOpaqueBatches();
        prepareAlphaBatches();

        if (Q_UNLIKELY(debug_build)) {
            qDebug() << "Opaque Batches:";
            for (int i=0; i<m_opaqueBatches.size(); ++i) {
                Batch *b = m_opaqueBatches.at(i);
                qDebug() << " - Batch " << i << b << (b->needsUpload ? "upload" : "") << " root:" << b->root;
                for (Element *e = b->first; e; e = e->nextInBatch) {
                    qDebug() << "   - element:" << e << " node:" << e->node << e->order;
                }
            }
            qDebug() << "Alpha Batches:";
            for (int i=0; i<m_alphaBatches.size(); ++i) {
                Batch *b = m_alphaBatches.at(i);
                qDebug() << " - Batch " << i << b << (b->needsUpload ? "upload" : "") << " root:" << b->root;
                for (Element *e = b->first; e; e = e->nextInBatch) {
                    qDebug() << "   - element:" << e << e->bounds << " node:" << e->node << " order:" << e->order;
                }
            }
        }
    }

    deleteRemovedElements();

    // Then sort opaque batches so that we're drawing the batches with the highest
    // order first, maximizing the benefit of front-to-back z-ordering.
    if (m_opaqueBatches.size())
        std::sort(&m_opaqueBatches.first(), &m_opaqueBatches.last() + 1, qsg_sort_batch_decreasing_order);

    // Sort alpha batches back to front so that they render correctly.
    if (m_alphaBatches.size())
        std::sort(&m_alphaBatches.first(), &m_alphaBatches.last() + 1, qsg_sort_batch_increasing_order);

    m_zRange = 1.0 / (m_nextRenderOrder);

    if (Q_UNLIKELY(debug_upload)) qDebug() << "Uploading Opaque Batches:";
    for (int i=0; i<m_opaqueBatches.size(); ++i)
        uploadBatch(m_opaqueBatches.at(i));

    if (Q_UNLIKELY(debug_upload)) qDebug() << "Uploading Alpha Batches:";
    for (int i=0; i<m_alphaBatches.size(); ++i)
        uploadBatch(m_alphaBatches.at(i));

    renderBatches();

    m_rebuild = 0;

    if (m_vao)
        m_vao->release();
}

void Renderer::prepareRenderNode(RenderNodeElement *e)
{
    if (e->fbo && e->fbo->size() != deviceRect().size()) {
        delete e->fbo;
        e->fbo = 0;
    }

    if (!e->fbo)
        e->fbo = new QOpenGLFramebufferObject(deviceRect().size(), QOpenGLFramebufferObject::CombinedDepthStencil);
    e->fbo->bind();

    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    QSGRenderNode::RenderState state;
    QMatrix4x4 pm = projectionMatrix();
    state.projectionMatrix = &pm;
    state.scissorEnabled = false;
    state.stencilEnabled = false;

    QSGNode *clip = e->renderNode->parent();
    e->renderNode->m_clip_list = 0;
    while (clip != rootNode()) {
        if (clip->type() == QSGNode::ClipNodeType) {
            e->renderNode->m_clip_list = static_cast<QSGClipNode *>(clip);
            break;
        }
        clip = clip->parent();
    }

    QSGNode *xform = e->renderNode->parent();
    QMatrix4x4 matrix;
    while (xform != rootNode()) {
        if (xform->type() == QSGNode::TransformNodeType) {
            matrix = matrixForRoot(e->root) * static_cast<QSGTransformNode *>(xform)->combinedMatrix();
            break;
        }
        xform = xform->parent();
    }
    e->renderNode->m_matrix = &matrix;

    QSGNode *opacity = e->renderNode->parent();
    e->renderNode->m_opacity = 1.0;
    while (opacity != rootNode()) {
        if (opacity->type() == QSGNode::OpacityNodeType) {
            e->renderNode->m_opacity = static_cast<QSGOpacityNode *>(opacity)->combinedOpacity();
            break;
        }
        opacity = opacity->parent();
    }

    e->renderNode->render(state);

    e->renderNode->m_matrix = 0;

    bindable()->bind();
}

void Renderer::renderRenderNode(Batch *batch)
{
    updateStencilClip(0);
    m_currentClip = 0;

    setActiveShader(0, 0);

    if (!m_shaderManager->blitProgram) {
        m_shaderManager->blitProgram = new QOpenGLShaderProgram();

        QSGShaderSourceBuilder::initializeProgramFromFiles(
            m_shaderManager->blitProgram,
            QStringLiteral(":/scenegraph/shaders/rendernode.vert"),
            QStringLiteral(":/scenegraph/shaders/rendernode.frag"));
        m_shaderManager->blitProgram->bindAttributeLocation("av", 0);
        m_shaderManager->blitProgram->bindAttributeLocation("at", 1);
        m_shaderManager->blitProgram->link();

        Q_ASSERT(m_shaderManager->blitProgram->isLinked());
    }

    RenderNodeElement *e = static_cast<RenderNodeElement *>(batch->first);
    glBindTexture(GL_TEXTURE_2D, e->fbo->texture());

    m_shaderManager->blitProgram->bind();

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    float z = 1.0f - e->order * m_zRange;

    float av[] = { -1, -1, z,
                    1, -1, z,
                   -1,  1, z,
                    1,  1, z };
    float at[] = { 0, 0,
                   1, 0,
                   0, 1,
                   1, 1 };

    glVertexAttribPointer(0, 3, GL_FLOAT, false, 0, av);
    glVertexAttribPointer(1, 2, GL_FLOAT, false, 0, at);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glBindTexture(GL_TEXTURE_2D, 0);
}

QT_END_NAMESPACE

}
