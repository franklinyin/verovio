/////////////////////////////////////////////////////////////////////////////
// Name:        editortoolkit_shared.h
// Author:      Laurent Pugin
// Created:     03/02/2026
// Copyright (c) Authors and others. All rights reserved.
/////////////////////////////////////////////////////////////////////////////

#ifndef __VRV_EDITOR_TOOLKIT_SHARED_H__
#define __VRV_EDITOR_TOOLKIT_SHARED_H__

#include <cmath>
#include <deque>
#include <string>
#include <utility>
#include <vector>

//--------------------------------------------------------------------------------

#include "doc.h"
#include "editortoolkit.h"
#include "view.h"

#include "jsonxx.h"

namespace vrv {

class EditorTreeObject;

//--------------------------------------------------------------------------------
// EditorToolkitShared
//--------------------------------------------------------------------------------

class EditorToolkitShared : public EditorToolkit {
public:
    EditorToolkitShared(Doc *doc, View *view);
    virtual ~EditorToolkitShared();
    bool ParseEditorAction(const std::string &json_editorAction) override
    {
        return ParseEditorAction(json_editorAction, false);
    }
    bool ParseEditorAction(const std::string &json_editorAction, bool commitOnly = false);

protected:
#ifndef NO_EDIT_SUPPORT
    /**
     * Parse JSON instructions for experimental editor functions.
     */
    ///@{
    bool Chain(jsonxx::Array actions);
    bool ParseContextAction(jsonxx::Object param, std::string &elementId, bool &scores, bool &sections);
    bool ParseDeleteAction(jsonxx::Object param, std::string &elementId);
    bool ParseDragAction(jsonxx::Object param, std::string &elementId, int &x, int &y);
    bool ParseKeyDownAction(jsonxx::Object param, std::string &elementid, int &key, bool &shiftKey, bool &ctrlKey);
    bool ParseInsertAction(
        jsonxx::Object param, std::string &elementName, std::string &elementId, std::string &insertMode);
    bool IsSchenkerNoteInsert(const jsonxx::Object &param) const;
    bool IsSchenkerNoteDelete(const jsonxx::Object &param);
    bool IsSchenkerBeamAction(const jsonxx::Object &param);
    bool IsSchenkerFlipAction(const jsonxx::Object &param);
    bool IsSchenkerSlurAction(const jsonxx::Object &param);
    bool IsSchenkerSlurBezierAction(const jsonxx::Object &param);
    bool IsSchenkerSlurCurveAction(const jsonxx::Object &param);
    bool IsSchenkerSlurResetAction(const jsonxx::Object &param);
    bool IsSchenkerOverlayChain(const jsonxx::Array &actions);
    bool ParseBeamAction(jsonxx::Object param, std::vector<std::string> &noteIds);
    bool ParseFlipAction(jsonxx::Object param, std::string &elementId);
    bool ParseSlurAction(jsonxx::Object param, std::vector<std::string> &noteIds);
    bool ParseSlurBezierAction(jsonxx::Object param, std::string &elementId, Point points[4]);
    bool ParseSchenkerSlurCurveAction(jsonxx::Object param, std::string &elementId, Point points[4]);
    bool ParseSchenkerNoteInsertAction(jsonxx::Object param, std::string &staffId, int &loc,
        double &schenkerX, int &dur, bool &voidHead, bool &showStem);
    bool ParseInsertControlAction(
        jsonxx::Object param, std::string &elementName, std::string &startId, std::string &endId);
    bool ParseNavigate(jsonxx::Object param, std::string &elementId, int &direction);
    bool ParsePropertiesAction(jsonxx::Object param, std::string &scoreDef);
    bool ParseSetAction(jsonxx::Object param, std::string &elementId, std::string &attribute, std::string &value);
    ///@}

    void SetEditInfo();
    void PrepareUndo();
    std::string GetCurrentState();
    bool ReloadState(const std::string &data);
    void TrimUndoMemory();
    bool CanUndo() const;
    bool CanRedo() const;
    bool Undo();
    bool Redo();

    /**
     * Experimental editor functions.
     */
    ///@{
    bool Delete(std::string &elementId);
    bool BeamSchenkerNotes(const std::vector<std::string> &noteIds);
    bool SlurSchenkerNotes(const std::vector<std::string> &noteIds);
    bool SetSchenkerSlurBezier(const std::string &elementId, const Point points[4]);
    bool SetSchenkerSlurCurve(const std::string &elementId, const Point devicePoints[4]);
    bool ResetSchenkerSlurCurve(const std::string &elementId);
    bool FlipSchenker(const std::string &elementId);
    bool Drag(std::string &elementId, int x, int y);
    bool InsertSchenkerNote(
        const std::string &staffId, int loc, double schenkerX, int dur, bool voidHead, bool showStem);
    bool InsertControl(const std::string &elementName, const std::string startId, const std::string endId);
    bool KeyDown(std::string &elementId, int key, bool shiftKey, bool ctrlKey);
    bool Navigate(std::string &elementId, const int &direction);
    bool Set(std::string &elementId, std::string const &attribute, std::string const &value);
    ///@}

    void ClearContext();
    bool ContextForElement(std::string &elementId);
    bool ContextForScores(bool editInfo);
    bool ContextForSections(bool editInfo);

    bool GetScoreDef();
    bool SetScoreDef(const std::string scoreDef);

    void ContextForObject(const Object *object, jsonxx::Object &element, bool recursive = false);
    void ContextForObjects(const ArrayOfConstObjects &objects, jsonxx::Array &siblings);
    void ContextForReferences(const ListOfObjectAttNamePairs &objects, jsonxx::Array &links);

    ArrayOfConstObjects GetScoreBasedChildrenFor(const Object *object);

    void CollectReferringObjects(
        const Object *element, std::set<std::string> &toDelete, std::set<const Object *> &visited);

public:
    //
protected:
    bool m_undoPrepared;
    std::deque<std::string> m_undoStack;
    std::deque<std::string> m_redoStack;
    size_t m_undoMemoryUsage = 0;

    EditorTreeObject *m_scoreContext;
    EditorTreeObject *m_sectionContext;
    EditorTreeObject *m_currentContext;
};

//----------------------------------------------------------------------------
// EditorTreeObject
//----------------------------------------------------------------------------

/**
 * This class stores an alignment position elements will point to
 */
class EditorTreeObject : public Object, public VisibilityDrawingInterface {

public:
    /**
     * @name Constructors, destructors, reset methods
     * Reset method resets all attribute classes
     */
    ///@{
    EditorTreeObject(const Object *object, bool ownChildren);
    virtual ~EditorTreeObject() {};
    void Reset() override;
    std::string GetClassName() const override { return m_className; }
    ///@}

    /**
     * @name Methods for adding allowed content
     */
    ///@{
    bool IsSupportedChild(ClassId classId) override { return true; }
    ///@}

    /**
     * @name Getter to interfaces
     */
    ///@{
    VisibilityDrawingInterface *GetVisibilityDrawingInterface() override
    {
        return vrv_cast<VisibilityDrawingInterface *>(this);
    }
    const VisibilityDrawingInterface *GetVisibilityDrawingInterface() const override
    {
        return vrv_cast<const VisibilityDrawingInterface *>(this);
    }
    ///@}

    ArrayOfConstObjects GetChildObjects() const;

private:
    //
public:
    std::string m_className;
    const Object *m_object;

private:
    //
#endif /* NO_EDIT_SUPPORT */
};

} // namespace vrv

#endif
