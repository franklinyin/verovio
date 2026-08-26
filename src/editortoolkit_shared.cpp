/////////////////////////////////////////////////////////////////////////////
// Name:        editortoolkit_shared.cpp
// Author:      Laurent Pugin
// Created:     03/02/2026
// Copyright (c) Authors and others. All rights reserved.
/////////////////////////////////////////////////////////////////////////////

#include "editortoolkit_shared.h"

//--------------------------------------------------------------------------------

#include <algorithm>
#include <climits>
#include <cmath>
#include <exception>
#include <locale>
#include <set>
#include <sstream>
#include <string>
#include <vector>

//--------------------------------------------------------------------------------

#include "beam.h"
#include "chord.h"
#include "clef.h"
#include "comparison.h"
#include "dir.h"
#include "dynam.h"
#include "editfunctor.h"
#include "editorial.h"
#include "findfunctor.h"
#include "hairpin.h"
#include "iomei.h"
#include "layer.h"
#include "mdiv.h"
#include "measure.h"
#include "mnum.h"
#include "note.h"
#include "page.h"
#include "preparedatafunctor.h"
#include "pages.h"
#include "plistinterface.h"
#include "rend.h"
#include "rest.h"
#include "slur.h"
#include "staff.h"
#include "stem.h"
#include "surface.h"
#include "symboldef.h"
#include "system.h"
#include "systemelement.h"
#include "text.h"
#include "tie.h"
#include "timeinterface.h"
#include "vrv.h"
#include "zone.h"

//--------------------------------------------------------------------------------

#define UNDO_MEMORY_LIMIT (256 * 1024 * 1024) // 256 MB

namespace vrv {

namespace {

bool IsSchenkerMovableNote(const Note *note)
{
    return note && note->IsSchenker();
}

void SetSchenkerX(Note *note, double schenkerX)
{
    const std::string xStr = std::to_string(schenkerX);
    bool updated = false;
    for (auto &pair : note->m_unsupported) {
        if (pair.first == "schenker:x") {
            pair.second = xStr;
            updated = true;
            break;
        }
    }
    if (!updated) note->m_unsupported.push_back(std::make_pair("schenker:x", xStr));
    note->SetDrawingFreeXFromGraphical(schenkerX);
}

bool IsSchenkerBeamableNote(const Note *note)
{
    if (!note || !note->IsSchenker()) return false;
    if (note->IsInBeam()) return false;
    if (note->GetActualDur() != DURATION_8) return false;
    const Stem *stem = vrv_cast<const Stem *>(note->FindDescendantByType(STEM, 1));
    if (!stem || (stem->GetVisible() == BOOLEAN_false)) return false;
    return true;
}

bool IsSchenkerStemmedNote(const Note *note)
{
    if (!note || !note->IsSchenker()) return false;
    if (note->IsInBeam()) return false;
    if (note->GetActualDur() == DURATION_1) return false;
    const Stem *stem = vrv_cast<const Stem *>(note->FindDescendantByType(STEM, 1));
    if (!stem || (stem->GetVisible() == BOOLEAN_false)) return false;
    return true;
}

bool IsSchenkerBeamElement(const Beam *beam)
{
    if (!beam) return false;
    ListOfConstObjects notes = beam->FindAllDescendantsByType(NOTE);
    if (notes.empty()) return false;
    for (const Object *object : notes) {
        const Note *note = vrv_cast<const Note *>(object);
        if (!note || !note->IsSchenker()) return false;
    }
    return true;
}

bool IsSchenkerSlurableNote(const Note *note)
{
    return note && note->IsSchenker();
}

bool IsSchenkerSlurElement(const Slur *slur)
{
    if (!slur) return false;
    const LayerElement *start = slur->GetStart();
    const LayerElement *end = slur->GetEnd();
    return start && end && start->IsSchenker() && end->IsSchenker();
}

data_STEMDIRECTION ResolveSchenkerStemDir(Note *note, Doc *doc, Staff *staff)
{
    if (note->HasStemDir()) return note->GetStemDir();
    if (note->GetDrawingStemDir() != STEMDIRECTION_NONE) return note->GetDrawingStemDir();
    Stem *stem = note->GetDrawingStem();
    if (stem && stem->HasDir()) return stem->GetDir();
    if (staff && doc) {
        const int verticalCenter
            = staff->GetDrawingY() - doc->GetDrawingUnit(staff->m_drawingStaffSize) * (staff->m_drawingLines - 1);
        return (note->GetDrawingY() >= verticalCenter) ? STEMDIRECTION_down : STEMDIRECTION_up;
    }
    return STEMDIRECTION_up;
}

void ApplyStemDirToNote(Note *note, data_STEMDIRECTION dir)
{
    note->SetStemDir(dir);
    note->SetDrawingStemDir(dir);
    if (Stem *stem = note->GetDrawingStem()) {
        stem->SetDir(dir);
    }
}

data_BEAMPLACE ResolveSchenkerBeamPlace(Beam *beam, Doc *doc, Staff *staff)
{
    if (beam->HasPlace()) return beam->GetPlace();
    if (beam->m_drawingPlace != BEAMPLACE_NONE) return beam->m_drawingPlace;
    const Note *first = vrv_cast<const Note *>(beam->FindDescendantByType(NOTE));
    if (first && staff) {
        const data_STEMDIRECTION dir
            = ResolveSchenkerStemDir(const_cast<Note *>(first), doc, staff);
        return (dir == STEMDIRECTION_up) ? BEAMPLACE_above : BEAMPLACE_below;
    }
    return BEAMPLACE_below;
}

void LogSchenkerStaffGeometry(const char *stage, Doc *doc, Staff *staff)
{
    if (!staff) {
        LogInfo("Schenker staff [%s]: staff=null", stage);
        return;
    }
    Page *page = doc ? doc->GetDrawingPage() : NULL;
    Zone *zone = staff->GetZone();
    LogInfo(
        "Schenker staff [%s] id=%s size=%d facsY=%d ppu=%f page=%dx%d zone=%s", stage, staff->GetID().c_str(),
        staff->m_drawingStaffSize, staff->m_drawingFacsY, page ? page->GetPPUFactor() : -1.0,
        page ? page->m_pageWidth : -1, page ? page->m_pageHeight : -1,
        zone ? StringFormat("%d,%d,%d,%d", zone->GetUlx(), zone->GetUly(), zone->GetLrx(), zone->GetLry()).c_str()
             : "none");
}

} // namespace

EditorToolkitShared::EditorToolkitShared(Doc *doc, View *view) : EditorToolkit(doc, view)
{
    m_undoPrepared = false;
    m_scoreContext = NULL;
    m_sectionContext = NULL;
    m_currentContext = NULL;

    this->SetEditInfo();
}

EditorToolkitShared::~EditorToolkitShared()
{
#ifndef NO_EDIT_SUPPORT
    this->ClearContext();
#endif
}

bool EditorToolkitShared::ParseEditorAction(const std::string &json_editorAction, bool commitOnly)
{
#ifndef NO_EDIT_SUPPORT
    jsonxx::Object json;

    // Read JSON actions
    if (!json.parse(json_editorAction)) {
        LogError("Cannot parse JSON std::string.");
        return false;
    }

    if (!json.has<jsonxx::String>("action")) {
        LogWarning("Incorrectly formatted JSON action.");
    }

    std::string action = json.get<jsonxx::String>("action");

    // Stage-1 Schenker insert/delete is an overlay on already-established
    // transcription geometry. Do not run the generic CMN SetFocus() cycle
    // (PrepareData / ScoreDefSetCurrentDoc / RefreshLayout).
    bool skipSetFocus = false;
    if ((action == "insert") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerNoteInsert(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "delete") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerNoteDelete(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "beam") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerBeamAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "flip") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerFlipAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "slur") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerSlurAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "slurBezier") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerSlurBezierAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "schenkerSlurCurve") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerSlurCurveAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "schenkerSlurReset") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerSlurResetAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "schenkerNoteMove") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerNoteMoveAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "schenkerLabel") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerLabelAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "schenkerLabelOffset") && json.has<jsonxx::Object>("param")) {
        skipSetFocus = this->IsSchenkerLabelOffsetAction(json.get<jsonxx::Object>("param"));
    }
    else if ((action == "chain") && json.has<jsonxx::Array>("param")) {
        skipSetFocus = this->IsSchenkerOverlayChain(json.get<jsonxx::Array>("param"));
    }

    if (skipSetFocus) {
        Staff *probe = NULL;
        if ((action == "delete") && json.has<jsonxx::Object>("param")) {
            std::string elementId;
            if (this->ParseDeleteAction(json.get<jsonxx::Object>("param"), elementId)) {
                Object *target = this->GetElement(elementId);
                if (target) probe = dynamic_cast<Staff *>(target->GetFirstAncestor(STAFF));
            }
        }
        else if ((action == "insert") && json.has<jsonxx::Object>("param")) {
            jsonxx::Object param = json.get<jsonxx::Object>("param");
            std::string staffId;
            int loc = 0;
            double schenkerX = 0.0;
            int dur = 1;
            bool voidHead = false;
            bool showStem = false;
            if (this->ParseSchenkerNoteInsertAction(param, staffId, loc, schenkerX, dur, voidHead, showStem)) {
                Object *target = this->GetElement(staffId);
                probe = dynamic_cast<Staff *>(target);
                if (!probe && target) probe = dynamic_cast<Staff *>(target->GetFirstAncestor(STAFF));
            }
        }
        LogSchenkerStaffGeometry("A-before-SetFocus-skipped", m_doc, probe);
    }

    if (!skipSetFocus && (action != "context") && (action != "properties")) {
        m_doc->SetFocus();
    }

    // Action without parameter
    if (action == "commit") {
        m_doc->PrepareData();
        m_doc->ScoreDefSetCurrentDoc(true);
        m_doc->RefreshLayout();
        m_undoPrepared = false;
        this->SetEditInfo();
        return true;
    }

    // Undo and redo - also without parameter
    if ((action == "undo") || (action == "redo")) {
        this->ClearContext();
        if (action == "undo") {
            this->Undo();
        }
        else {
            this->Redo();
        }
        m_doc->PrepareData();
        m_doc->ScoreDefSetCurrentDoc(true);
        m_undoPrepared = false;
        this->SetEditInfo();
        return true;
    }

    if (commitOnly) {
        // Only process commit actions
        return false;
    }

    if (!json.has<jsonxx::Object>("param") && !json.has<jsonxx::Array>("param")) {
        LogWarning("Incorrectly formatted JSON param.");
    }

    if (action == "chain") {
        if (!json.has<jsonxx::Array>("param")) {
            LogError("Incorrectly formatted JSON action");
            return false;
        }
        return this->Chain(json.get<jsonxx::Array>("param"));
    }
    else if (action == "context") {
        std::string elementId;
        bool scores;
        bool sections;
        if (this->ParseContextAction(json.get<jsonxx::Object>("param"), elementId, scores, sections)) {
            if (scores) {
                return this->ContextForScores(true);
            }
            else if (sections) {
                return this->ContextForSections(true);
            }
            else {
                return this->ContextForElement(elementId);
            }
        }
        LogWarning("Could not parse the context action");
    }
    else if (action == "delete") {
        std::string elementId;
        if (this->ParseDeleteAction(json.get<jsonxx::Object>("param"), elementId)) {
            this->PrepareUndo();
            return (this->Delete(elementId));
        }
        LogWarning("Could not parse the delete action");
    }
    else if (action == "beam") {
        std::vector<std::string> noteIds;
        if (this->ParseBeamAction(json.get<jsonxx::Object>("param"), noteIds)) {
            this->PrepareUndo();
            return this->BeamSchenkerNotes(noteIds);
        }
        LogWarning("Could not parse the beam action");
    }
    else if (action == "flip") {
        std::string elementId;
        if (this->ParseFlipAction(json.get<jsonxx::Object>("param"), elementId)) {
            this->PrepareUndo();
            return this->FlipSchenker(elementId);
        }
        LogWarning("Could not parse the flip action");
    }
    else if (action == "slur") {
        std::vector<std::string> noteIds;
        if (this->ParseSlurAction(json.get<jsonxx::Object>("param"), noteIds)) {
            this->PrepareUndo();
            return this->SlurSchenkerNotes(noteIds);
        }
        LogWarning("Could not parse the slur action");
    }
    else if (action == "slurBezier") {
        std::string elementId;
        Point points[4];
        if (this->ParseSlurBezierAction(json.get<jsonxx::Object>("param"), elementId, points)) {
            this->PrepareUndo();
            return this->SetSchenkerSlurBezier(elementId, points);
        }
        LogWarning("Could not parse the slurBezier action");
    }
    else if (action == "schenkerSlurCurve") {
        std::string elementId;
        Point points[4];
        if (this->ParseSchenkerSlurCurveAction(json.get<jsonxx::Object>("param"), elementId, points)) {
            this->PrepareUndo();
            return this->SetSchenkerSlurCurve(elementId, points);
        }
        LogWarning("Could not parse the schenkerSlurCurve action");
    }
    else if (action == "schenkerSlurReset") {
        std::string elementId;
        if (this->ParseSchenkerSlurResetAction(json.get<jsonxx::Object>("param"), elementId)) {
            this->PrepareUndo();
            return this->ResetSchenkerSlur(elementId);
        }
        LogWarning("Could not parse the schenkerSlurReset action");
    }
    else if (action == "schenkerNoteMove") {
        std::string elementId;
        int loc = 0;
        double schenkerX = 0.0;
        if (this->ParseSchenkerNoteMoveAction(json.get<jsonxx::Object>("param"), elementId, loc, schenkerX)) {
            this->PrepareUndo();
            return this->MoveSchenkerNote(elementId, loc, schenkerX);
        }
        LogWarning("Could not parse the schenkerNoteMove action");
    }
    else if (action == "schenkerLabel") {
        std::string noteId;
        std::string text;
        if (this->ParseSchenkerLabelAction(json.get<jsonxx::Object>("param"), noteId, text)) {
            this->PrepareUndo();
            return this->InsertSchenkerLabel(noteId, text);
        }
        LogWarning("Could not parse the schenkerLabel action");
    }
    else if (action == "schenkerLabelOffset") {
        std::string elementId;
        Point fromDevice;
        Point toDevice;
        if (this->ParseSchenkerLabelOffsetAction(
                json.get<jsonxx::Object>("param"), elementId, fromDevice, toDevice)) {
            this->PrepareUndo();
            return this->SetSchenkerLabelOffset(elementId, fromDevice, toDevice);
        }
        LogWarning("Could not parse the schenkerLabelOffset action");
    }
    else if (action == "drag") {
        std::string elementId;
        int x, y;
        if (this->ParseDragAction(json.get<jsonxx::Object>("param"), elementId, x, y)) {
            this->PrepareUndo();
            return (this->Drag(elementId, x, y));
        }
        LogWarning("Could not parse the drag action");
    }
    else if (action == "insert") {
        jsonxx::Object param = json.get<jsonxx::Object>("param");
        if (this->IsSchenkerNoteInsert(param)) {
            std::string staffId;
            int loc = 0;
            double schenkerX = 0.0;
            int dur = 1;
            bool voidHead = false;
            bool showStem = false;
            if (this->ParseSchenkerNoteInsertAction(param, staffId, loc, schenkerX, dur, voidHead, showStem)) {
                this->PrepareUndo();
                return this->InsertSchenkerNote(staffId, loc, schenkerX, dur, voidHead, showStem);
            }
            LogWarning("Could not parse the Schenker insert action: %s", param.json().c_str());
        }
        else {
            std::string elementName, elementId, insertMode;
            if (this->ParseInsertAction(param, elementName, elementId, insertMode)) {
                this->PrepareUndo();
                // LogInfo("%s %s %s", elementName.c_str(), elementId.c_str(), insertMode.c_str());
                if (insertMode == "appendChild") {
                    return (this->AppendChild(elementId, elementName, false));
                }
                else if (insertMode == "appendChildNoDuplicate") {
                    return (this->AppendChild(elementId, elementName, true));
                }
                else if (insertMode == "insertBefore") {
                    return (this->InsertBefore(elementId, elementName));
                }
                else if (insertMode == "insertAfter") {
                    return (this->InsertAfter(elementId, elementName));
                }
            }
            LogWarning("Could not parse the insert action");
        }
    }
    else if (action == "insertControl") {
        std::string elementName, startId, endId;
        if (this->ParseInsertControlAction(json.get<jsonxx::Object>("param"), elementName, startId, endId)) {
            this->PrepareUndo();
            // LogInfo("%s %s %s", elementName.c_str(), elementId.c_str(), insertMode.c_str());
            return (this->InsertControl(elementName, startId, endId));
        }
        LogWarning("Could not parse the insertControl action");
    }
    else if (action == "keyDown") {
        std::string elementId;
        int key;
        bool shiftKey, ctrlKey;
        if (this->ParseKeyDownAction(json.get<jsonxx::Object>("param"), elementId, key, shiftKey, ctrlKey)) {
            this->PrepareUndo();
            return (this->KeyDown(elementId, key, shiftKey, ctrlKey));
        }
        LogWarning("Could not parse the keyDown action");
    }
    else if (action == "navigate") {
        std::string elementId;
        int direction;
        if (this->ParseNavigate(json.get<jsonxx::Object>("param"), elementId, direction)) {
            return this->Navigate(elementId, direction);
        }
        LogWarning("Could not parse the navigate action");
    }
    else if (action == "properties") {
        std::string scoreDef;
        if (this->ParsePropertiesAction(json.get<jsonxx::Object>("param"), scoreDef)) {
            if (scoreDef.empty()) {
                return this->GetScoreDef();
            }
            else {
                return this->SetScoreDef(scoreDef);
            }
        }
    }
    else if (action == "set") {
        std::string elementId, attribute, value;
        if (this->ParseSetAction(json.get<jsonxx::Object>("param"), elementId, attribute, value)) {
            this->PrepareUndo();
            return (this->Set(elementId, attribute, value));
        }
        LogWarning("Could not parse the set action");
    }
    else {
        LogWarning("Unknown action type '%s'.", action.c_str());
    }
    return false;
#else /* NO_EDIT_SUPPORT */
    LogError("Editor functions are not supported in this build.");
    return false;
#endif /* NO_EDIT_SUPPORT */
}

#ifndef NO_EDIT_SUPPORT
bool EditorToolkitShared::ParseContextAction(jsonxx::Object param, std::string &elementId, bool &scores, bool &sections)
{
    scores = false;
    sections = false;
    if (param.has<jsonxx::String>("elementId")) {
        elementId = param.get<jsonxx::String>("elementId");
        return true;
    }
    else if (param.has<jsonxx::String>("document")) {
        scores = (param.get<jsonxx::String>("document") == "scores");
        sections = !scores;
        return true;
    }
    return false;
}

bool EditorToolkitShared::ParseDeleteAction(jsonxx::Object param, std::string &elementId)
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    return true;
}

bool EditorToolkitShared::ParseDragAction(jsonxx::Object param, std::string &elementId, int &x, int &y)
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    if (!param.has<jsonxx::Number>("x")) return false;
    x = param.get<jsonxx::Number>("x");
    if (!param.has<jsonxx::Number>("y")) return false;
    y = param.get<jsonxx::Number>("y");
    return true;
}

bool EditorToolkitShared::ParseInsertAction(
    jsonxx::Object param, std::string &elementName, std::string &elementId, std::string &insertMode)
{
    if (!param.has<jsonxx::String>("elementName")) return false;
    elementName = param.get<jsonxx::String>("elementName");
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    if (!param.has<jsonxx::String>("insertMode")) return false;
    insertMode = param.get<jsonxx::String>("insertMode");
    return true;
}

bool EditorToolkitShared::IsSchenkerNoteInsert(const jsonxx::Object &param) const
{
    if (!param.has<jsonxx::String>("elementType")) return false;
    if (param.get<jsonxx::String>("elementType") != "note") return false;
    if (!param.has<jsonxx::Object>("attributes")) return false;
    jsonxx::Object attributes = param.get<jsonxx::Object>("attributes");
    if (!attributes.has<jsonxx::String>("type")) return false;
    return attributes.get<jsonxx::String>("type") == "schenker";
}

bool EditorToolkitShared::IsSchenkerNoteDelete(const jsonxx::Object &param)
{
    std::string elementId;
    if (!this->ParseDeleteAction(param, elementId)) return false;
    Object *element = this->GetElement(elementId);
    if (!element) return false;
    if (element->Is(BEAM)) {
        return IsSchenkerBeamElement(dynamic_cast<Beam *>(element));
    }
    if (element->Is(SLUR)) {
        return IsSchenkerSlurElement(dynamic_cast<Slur *>(element));
    }
    LayerElement *layerElement = dynamic_cast<LayerElement *>(element);
    return layerElement && layerElement->IsSchenker();
}

bool EditorToolkitShared::ParseSlurAction(jsonxx::Object param, std::vector<std::string> &noteIds)
{
    noteIds.clear();
    if (!param.has<jsonxx::Array>("noteIds")) return false;
    jsonxx::Array ids = param.get<jsonxx::Array>("noteIds");
    if (ids.size() != 2) return false;
    for (int i = 0; i < (int)ids.size(); ++i) {
        if (!ids.has<jsonxx::String>(i)) return false;
        noteIds.push_back(ids.get<jsonxx::String>(i));
    }
    return true;
}

bool EditorToolkitShared::IsSchenkerSlurAction(const jsonxx::Object &param)
{
    std::vector<std::string> noteIds;
    if (!this->ParseSlurAction(param, noteIds)) return false;
    for (const std::string &elementId : noteIds) {
        Object *element = this->GetElement(elementId);
        Note *note = dynamic_cast<Note *>(element);
        if (!IsSchenkerSlurableNote(note)) return false;
    }
    return true;
}

bool EditorToolkitShared::ParseSlurBezierAction(
    jsonxx::Object param, std::string &elementId, Point points[4])
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    if (!param.has<jsonxx::Array>("points")) return false;
    jsonxx::Array pts = param.get<jsonxx::Array>("points");
    if (pts.size() != 4) return false;
    for (int i = 0; i < 4; ++i) {
        if (!pts.has<jsonxx::Array>(i)) return false;
        jsonxx::Array pair = pts.get<jsonxx::Array>(i);
        if (pair.size() != 2 || !pair.has<jsonxx::Number>(0) || !pair.has<jsonxx::Number>(1)) return false;
        points[i].x = (int)std::lround(pair.get<jsonxx::Number>(0));
        points[i].y = (int)std::lround(pair.get<jsonxx::Number>(1));
    }
    return true;
}

bool EditorToolkitShared::IsSchenkerSlurBezierAction(const jsonxx::Object &param)
{
    std::string elementId;
    Point points[4];
    if (!this->ParseSlurBezierAction(param, elementId, points)) return false;
    Object *element = this->GetElement(elementId);
    return IsSchenkerSlurElement(dynamic_cast<Slur *>(element));
}

bool EditorToolkitShared::ParseSchenkerSlurCurveAction(
    jsonxx::Object param, std::string &elementId, Point points[4])
{
    // Same wire format as S2 metadata / React draft: four [x,y] pairs in
    // SVG/device (.page-margin) coordinates.
    return this->ParseSlurBezierAction(param, elementId, points);
}

bool EditorToolkitShared::IsSchenkerSlurCurveAction(const jsonxx::Object &param)
{
    std::string elementId;
    Point points[4];
    if (!this->ParseSchenkerSlurCurveAction(param, elementId, points)) return false;
    Object *element = this->GetElement(elementId);
    return IsSchenkerSlurElement(dynamic_cast<Slur *>(element));
}

bool EditorToolkitShared::ParseSchenkerSlurResetAction(jsonxx::Object param, std::string &elementId)
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    return !elementId.empty();
}

bool EditorToolkitShared::IsSchenkerSlurResetAction(const jsonxx::Object &param)
{
    std::string elementId;
    if (!this->ParseSchenkerSlurResetAction(param, elementId)) return false;
    Object *element = this->GetElement(elementId);
    return IsSchenkerSlurElement(dynamic_cast<Slur *>(element));
}

bool EditorToolkitShared::ParseSchenkerNoteMoveAction(
    jsonxx::Object param, std::string &elementId, int &loc, double &schenkerX)
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    if (elementId.empty()) return false;
    if (param.has<jsonxx::Number>("loc")) {
        loc = static_cast<int>(std::lround(param.get<jsonxx::Number>("loc")));
    }
    else if (param.has<jsonxx::String>("loc")) {
        try {
            loc = std::stoi(param.get<jsonxx::String>("loc"));
        }
        catch (const std::exception &) {
            return false;
        }
    }
    else {
        return false;
    }
    if (param.has<jsonxx::Number>("schenkerX")) {
        schenkerX = param.get<jsonxx::Number>("schenkerX");
    }
    else if (param.has<jsonxx::String>("schenkerX")) {
        try {
            schenkerX = std::stod(param.get<jsonxx::String>("schenkerX"));
        }
        catch (const std::exception &) {
            return false;
        }
    }
    else {
        return false;
    }
    return true;
}

bool EditorToolkitShared::IsSchenkerNoteMoveAction(const jsonxx::Object &param)
{
    std::string elementId;
    int loc = 0;
    double schenkerX = 0.0;
    if (!this->ParseSchenkerNoteMoveAction(param, elementId, loc, schenkerX)) return false;
    Object *element = this->GetElement(elementId);
    return IsSchenkerMovableNote(dynamic_cast<Note *>(element));
}

bool EditorToolkitShared::ParseSchenkerLabelAction(
    jsonxx::Object param, std::string &noteId, std::string &text)
{
    if (!param.has<jsonxx::String>("noteId")) return false;
    noteId = param.get<jsonxx::String>("noteId");
    if (noteId.empty()) return false;
    if (!param.has<jsonxx::String>("text")) return false;
    text = param.get<jsonxx::String>("text");
    return !text.empty();
}

bool EditorToolkitShared::IsSchenkerLabelAction(const jsonxx::Object &param)
{
    std::string noteId;
    std::string text;
    if (!this->ParseSchenkerLabelAction(param, noteId, text)) return false;
    Object *element = this->GetElement(noteId);
    Note *note = dynamic_cast<Note *>(element);
    return note && note->IsSchenker();
}

static bool ParseDeviceCoord(const jsonxx::Object &param, const std::string &key, int &value)
{
    if (!param.has<jsonxx::Number>(key)) return false;
    value = static_cast<int>(std::lround(param.get<jsonxx::Number>(key)));
    return true;
}

bool EditorToolkitShared::ParseSchenkerLabelOffsetAction(
    jsonxx::Object param, std::string &elementId, Point &fromDevice, Point &toDevice)
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    if (elementId.empty()) return false;
    int fromX = 0;
    int fromY = 0;
    int toX = 0;
    int toY = 0;
    if (!ParseDeviceCoord(param, "fromX", fromX)) return false;
    if (!ParseDeviceCoord(param, "fromY", fromY)) return false;
    if (!ParseDeviceCoord(param, "toX", toX)) return false;
    if (!ParseDeviceCoord(param, "toY", toY)) return false;
    fromDevice = Point(fromX, fromY);
    toDevice = Point(toX, toY);
    return true;
}

bool EditorToolkitShared::IsSchenkerLabelOffsetAction(const jsonxx::Object &param)
{
    std::string elementId;
    Point fromDevice;
    Point toDevice;
    if (!this->ParseSchenkerLabelOffsetAction(param, elementId, fromDevice, toDevice)) return false;
    Object *element = this->GetElement(elementId);
    Dir *dir = dynamic_cast<Dir *>(element);
    if (!dir) return false;
    LayerElement *start = dir->GetStart();
    Note *note = dynamic_cast<Note *>(start);
    return note && note->IsSchenker();
}

bool EditorToolkitShared::ParseBeamAction(jsonxx::Object param, std::vector<std::string> &noteIds)
{
    noteIds.clear();
    if (!param.has<jsonxx::Array>("noteIds")) return false;
    jsonxx::Array ids = param.get<jsonxx::Array>("noteIds");
    if (ids.size() < 2) return false;
    for (int i = 0; i < (int)ids.size(); ++i) {
        if (!ids.has<jsonxx::String>(i)) return false;
        noteIds.push_back(ids.get<jsonxx::String>(i));
    }
    return true;
}

bool EditorToolkitShared::IsSchenkerBeamAction(const jsonxx::Object &param)
{
    std::vector<std::string> noteIds;
    if (!this->ParseBeamAction(param, noteIds)) return false;
    for (const std::string &elementId : noteIds) {
        Object *element = this->GetElement(elementId);
        Note *note = dynamic_cast<Note *>(element);
        if (!IsSchenkerBeamableNote(note)) return false;
    }
    return true;
}

bool EditorToolkitShared::ParseFlipAction(jsonxx::Object param, std::string &elementId)
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    return !elementId.empty();
}

bool EditorToolkitShared::IsSchenkerFlipAction(const jsonxx::Object &param)
{
    std::string elementId;
    if (!this->ParseFlipAction(param, elementId)) return false;
    Object *element = this->GetElement(elementId);
    if (!element) return false;
    if (element->Is(NOTE)) {
        return IsSchenkerStemmedNote(dynamic_cast<Note *>(element));
    }
    if (element->Is(BEAM)) {
        return IsSchenkerBeamElement(dynamic_cast<Beam *>(element));
    }
    return false;
}

bool EditorToolkitShared::IsSchenkerOverlayChain(const jsonxx::Array &actions)
{
    if (actions.size() < 1) return false;
    for (int i = 0; i < (int)actions.size(); ++i) {
        if (!actions.has<jsonxx::Object>(i)) return false;
        jsonxx::Object step = actions.get<jsonxx::Object>(i);
        if (!step.has<jsonxx::String>("action") || !step.has<jsonxx::Object>("param")) return false;
        const std::string stepAction = step.get<jsonxx::String>("action");
        const jsonxx::Object stepParam = step.get<jsonxx::Object>("param");
        if (stepAction == "insert") {
            if (!this->IsSchenkerNoteInsert(stepParam)) return false;
        }
        else if (stepAction == "delete") {
            if (!this->IsSchenkerNoteDelete(stepParam)) return false;
        }
        else if (stepAction == "beam") {
            if (!this->IsSchenkerBeamAction(stepParam)) return false;
        }
        else if (stepAction == "flip") {
            if (!this->IsSchenkerFlipAction(stepParam)) return false;
        }
        else if (stepAction == "slur") {
            if (!this->IsSchenkerSlurAction(stepParam)) return false;
        }
        else if (stepAction == "slurBezier") {
            if (!this->IsSchenkerSlurBezierAction(stepParam)) return false;
        }
        else if (stepAction == "schenkerSlurCurve") {
            if (!this->IsSchenkerSlurCurveAction(stepParam)) return false;
        }
        else if (stepAction == "schenkerSlurReset") {
            if (!this->IsSchenkerSlurResetAction(stepParam)) return false;
        }
        else if (stepAction == "schenkerNoteMove") {
            if (!this->IsSchenkerNoteMoveAction(stepParam)) return false;
        }
        else if (stepAction == "schenkerLabel") {
            if (!this->IsSchenkerLabelAction(stepParam)) return false;
        }
        else if (stepAction == "schenkerLabelOffset") {
            if (!this->IsSchenkerLabelOffsetAction(stepParam)) return false;
        }
        else {
            return false;
        }
    }
    return true;
}

bool EditorToolkitShared::ParseSchenkerNoteInsertAction(jsonxx::Object param, std::string &staffId, int &loc,
    double &schenkerX, int &dur, bool &voidHead, bool &showStem)
{
    dur = 1;
    voidHead = false;
    showStem = false;
    if (!param.has<jsonxx::String>("elementType")) return false;
    if (param.get<jsonxx::String>("elementType") != "note") return false;
    if (!param.has<jsonxx::String>("staffId")) return false;
    staffId = param.get<jsonxx::String>("staffId");
    // ulx/uly are required by the Neon payload for API compatibility, but may
    // arrive as number or numeric string. They are not the stored position.
    if (!(param.has<jsonxx::Number>("ulx") || param.has<jsonxx::String>("ulx"))) return false;
    if (!(param.has<jsonxx::Number>("uly") || param.has<jsonxx::String>("uly"))) return false;
    if (!param.has<jsonxx::Object>("attributes")) return false;

    jsonxx::Object attributes = param.get<jsonxx::Object>("attributes");
    if (!attributes.has<jsonxx::String>("type")) return false;
    if (attributes.get<jsonxx::String>("type") != "schenker") return false;

    if (attributes.has<jsonxx::String>("loc")) {
        try {
            loc = std::stoi(attributes.get<jsonxx::String>("loc"));
        }
        catch (const std::exception &) {
            return false;
        }
    }
    else if (attributes.has<jsonxx::Number>("loc")) {
        loc = static_cast<int>(attributes.get<jsonxx::Number>("loc"));
    }
    else {
        return false;
    }

    if (attributes.has<jsonxx::String>("schenker:x")) {
        try {
            schenkerX = std::stod(attributes.get<jsonxx::String>("schenker:x"));
        }
        catch (const std::exception &) {
            return false;
        }
    }
    else if (attributes.has<jsonxx::Number>("schenker:x")) {
        schenkerX = attributes.get<jsonxx::Number>("schenker:x");
    }
    else {
        return false;
    }

    if (attributes.has<jsonxx::String>("dur")) {
        try {
            dur = std::stoi(attributes.get<jsonxx::String>("dur"));
        }
        catch (const std::exception &) {
            return false;
        }
    }
    else if (attributes.has<jsonxx::Number>("dur")) {
        dur = static_cast<int>(attributes.get<jsonxx::Number>("dur"));
    }

    if (attributes.has<jsonxx::String>("head.fill")) {
        voidHead = (attributes.get<jsonxx::String>("head.fill") == "void");
    }

    if (dur == 2 || dur == 8) {
        showStem = true;
    }
    if (attributes.has<jsonxx::String>("stem.visible")) {
        showStem = (attributes.get<jsonxx::String>("stem.visible") == "true");
    }
    else if (attributes.has<jsonxx::Boolean>("stem.visible")) {
        showStem = attributes.get<jsonxx::Boolean>("stem.visible");
    }

    return true;
}

bool EditorToolkitShared::ParseInsertControlAction(
    jsonxx::Object param, std::string &elementName, std::string &startId, std::string &endId)
{
    if (!param.has<jsonxx::String>("elementName")) return false;
    elementName = param.get<jsonxx::String>("elementName");
    if (!param.has<jsonxx::String>("startId")) return false;
    startId = param.get<jsonxx::String>("startId");
    if (!param.has<jsonxx::String>("endId")) return true;
    endId = param.get<jsonxx::String>("endId");
    return true;
}

bool EditorToolkitShared::ParseKeyDownAction(
    jsonxx::Object param, std::string &elementId, int &key, bool &shiftKey, bool &ctrlKey)
{
    // assign optional member
    shiftKey = false;
    ctrlKey = false;

    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    if (!param.has<jsonxx::Number>("key")) return false;
    key = param.get<jsonxx::Number>("key");
    // optional
    if (param.has<jsonxx::Boolean>("shiftKey")) {
        shiftKey = param.get<jsonxx::Boolean>("shiftKey");
    }
    if (param.has<jsonxx::Boolean>("ctrlKey")) {
        ctrlKey = param.get<jsonxx::Boolean>("ctrlKey");
    }
    return true;
}

bool EditorToolkitShared::ParseNavigate(jsonxx::Object param, std::string &elementId, int &direction)
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    if (!param.has<jsonxx::Number>("direction")) return false;
    direction = param.get<jsonxx::Number>("direction");
    return true;
}

bool EditorToolkitShared::ParsePropertiesAction(jsonxx::Object param, std::string &scoreDef)
{
    scoreDef = "";
    if (param.has<jsonxx::String>("scoreDef")) {
        scoreDef = param.get<jsonxx::String>("scoreDef");
        return true;
    }
    return true;
}

bool EditorToolkitShared::ParseSetAction(
    jsonxx::Object param, std::string &elementId, std::string &attribute, std::string &value)
{
    if (!param.has<jsonxx::String>("elementId")) return false;
    elementId = param.get<jsonxx::String>("elementId");
    if (!param.has<jsonxx::String>("attribute")) return false;
    attribute = param.get<jsonxx::String>("attribute");
    if (!param.has<jsonxx::String>("value")) return false;
    value = param.get<jsonxx::String>("value");
    return true;
}

void EditorToolkitShared::PrepareUndo()
{
    // We already have a prepared undo - nothing to prepare
    if (m_undoPrepared) return;

    std::string state = this->GetCurrentState();
    m_undoStack.push_back(state);
    m_undoMemoryUsage += state.size();
    // When new edit happens, redo stack is cleared
    while (!m_redoStack.empty()) {
        m_undoMemoryUsage -= m_redoStack.back().size();
        m_redoStack.pop_back();
    }
    TrimUndoMemory();
    // Set the flag
    m_undoPrepared = true;
}

void EditorToolkitShared::SetEditInfo()
{
    m_editInfo.reset();
    m_editInfo.import("chainedId", m_chainedId);
    m_editInfo.import("canUndo", this->CanUndo());
    m_editInfo.import("canRedo", this->CanRedo());
    m_editInfo.import("isMensuralMusicOnly", m_doc->IsMensuralMusicOnly());
}

std::string EditorToolkitShared::GetCurrentState()
{
    MEIOutput meioutput(m_doc);
    meioutput.SetSerializing(true);
    meioutput.SetBasic(false);
    meioutput.SetScoreBasedMEI(false);
    return meioutput.Export();
}

bool EditorToolkitShared::ReloadState(const std::string &data)
{
    MEIInput meiinput(m_doc);
    meiinput.SetDeserializing(true);
    return meiinput.Import(data);
}

bool EditorToolkitShared::CanUndo() const
{
    return (!m_undoStack.empty());
}

bool EditorToolkitShared::CanRedo() const
{
    return (!m_redoStack.empty());
}

bool EditorToolkitShared::Undo()
{
    if (!CanUndo()) return false;

    std::string currentState = this->GetCurrentState();
    m_redoStack.push_back(currentState);

    // Pop the previous state from undo stack
    std::string previous = m_undoStack.back();
    m_undoStack.pop_back();

    return ReloadState(previous);
}

bool EditorToolkitShared::Redo()
{
    if (!CanRedo()) return false;

    std::string currentState = this->GetCurrentState();
    m_undoStack.push_back(currentState);

    // Pop redo state and load it
    std::string redoState = m_redoStack.back();
    m_redoStack.pop_back();

    return ReloadState(redoState);
}

void EditorToolkitShared::TrimUndoMemory()
{
    // Drop the oldest undo entries if we exceed the limit
    while ((m_undoMemoryUsage > UNDO_MEMORY_LIMIT) && !m_undoStack.empty()) {
        m_undoMemoryUsage -= m_undoStack.front().size();
        m_undoStack.pop_front();
    }
    LogInfo("Undo stack size: %dMB", m_undoMemoryUsage / 1024 / 1024);
}

bool EditorToolkitShared::Chain(jsonxx::Array actions)
{
    bool status = true;
    m_chainedId = "";
    for (int i = 0; i < (int)actions.size(); ++i) {
        status = this->ParseEditorAction(actions.get<jsonxx::Object>(i).json(), !status);
    }
    return status;
}

bool EditorToolkitShared::Delete(std::string &elementId)
{
    Object *element = this->GetChainedElement(elementId);

    if (!element) return false;

    LayerElement *layerElement = dynamic_cast<LayerElement *>(element);
    const bool schenkerNote = layerElement && layerElement->IsSchenker();
    const bool schenkerBeam = element->Is(BEAM) && IsSchenkerBeamElement(dynamic_cast<Beam *>(element));
    const bool schenkerSlur = element->Is(SLUR) && IsSchenkerSlurElement(dynamic_cast<Slur *>(element));
    const bool schenkerOverlayDelete = schenkerNote || schenkerBeam || schenkerSlur;
    Staff *schenkerStaff = NULL;
    if (schenkerOverlayDelete) {
        schenkerStaff = dynamic_cast<Staff *>(element->GetFirstAncestor(STAFF));
        LogSchenkerStaffGeometry("F-before-schenker-delete", m_doc, schenkerStaff);
    }

    this->Navigate(elementId, 37);
    if (m_chainedId.empty() && element->GetParent()) m_chainedId = element->GetParent()->GetID();

    // Find referring objects
    std::set<std::string> objectsToDelete;
    SetOfConstObjects visited;
    objectsToDelete.insert(element->GetID());
    this->CollectReferringObjects(element, objectsToDelete, visited);
    for (auto id : objectsToDelete) {
        Object *toDelete = m_doc->FindDescendantByID(id);
        if (toDelete && toDelete->GetParent()) toDelete->GetParent()->DeleteChild(toDelete);
    }

    if (!m_chainedId.empty() && !m_doc->FindDescendantByID(m_chainedId)) m_chainedId = "";

    if (schenkerOverlayDelete) {
        if (Page *page = m_doc->GetDrawingPage()) {
            page->DeprecateLayout();
        }
        LogSchenkerStaffGeometry("G-after-schenker-delete", m_doc, schenkerStaff);
    }

    this->ClearContext();
    this->SetEditInfo();
    return true;
}

bool EditorToolkitShared::BeamSchenkerNotes(const std::vector<std::string> &noteIds)
{
    if (noteIds.size() < 2) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Beam requires at least two notes.");
        return false;
    }

    std::vector<Note *> notes;
    Layer *layer = NULL;
    Staff *staff = NULL;

    for (const std::string &elementId : noteIds) {
        Object *element = this->GetElement(elementId);
        Note *note = dynamic_cast<Note *>(element);
        if (!IsSchenkerBeamableNote(note)) {
            LogError("Note '%s' is not a beamable Schenker flagged note", elementId.c_str());
            m_editInfo.import("status", "FAILURE");
            m_editInfo.import("message", "Selected notes must be unbeamed Schenker eighth notes.");
            return false;
        }

        Layer *noteLayer = vrv_cast<Layer *>(note->GetFirstAncestor(LAYER));
        if (!noteLayer) {
            m_editInfo.import("status", "FAILURE");
            m_editInfo.import("message", "Could not find layer for selected notes.");
            return false;
        }

        if (!layer) {
            layer = noteLayer;
            staff = vrv_cast<Staff *>(layer->GetFirstAncestor(STAFF));
        }
        else if (layer != noteLayer) {
            LogError("Schenker beam notes must share the same layer");
            m_editInfo.import("status", "FAILURE");
            m_editInfo.import("message", "Selected notes must be on the same staff layer.");
            return false;
        }

        notes.push_back(note);
    }

    std::stable_sort(notes.begin(), notes.end(), [](Note *a, Note *b) {
        return a->GetDrawingFreeX() < b->GetDrawingFreeX();
    });

    int minIdx = INT_MAX;
    for (Note *note : notes) {
        minIdx = std::min(minIdx, layer->GetChildIndex(note));
    }

    std::vector<Note *> byLayerIndex = notes;
    std::stable_sort(byLayerIndex.begin(), byLayerIndex.end(), [layer](Note *a, Note *b) {
        return layer->GetChildIndex(a) > layer->GetChildIndex(b);
    });
    for (Note *note : byLayerIndex) {
        const int idx = layer->GetChildIndex(note);
        layer->DetachChild(idx);
    }

    Beam *beam = new Beam();
    layer->InsertChild(beam, minIdx);
    for (Note *note : notes) {
        beam->AddChild(note);
    }

    PrepareLayerElementPartsFunctor prepareParts;
    beam->Process(prepareParts);

    layer->ReorderByXPos();
    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }

    LogSchenkerStaffGeometry("H-after-schenker-beam", m_doc, staff);
    this->SetEditInfo();
    m_editInfo.import("uuid", beam->GetID());
    m_editInfo.import("status", "OK");
    return true;
}

bool EditorToolkitShared::SlurSchenkerNotes(const std::vector<std::string> &noteIds)
{
    if (noteIds.size() != 2) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Slur requires exactly two notes.");
        return false;
    }

    std::vector<Note *> notes;
    for (const std::string &elementId : noteIds) {
        Object *element = this->GetElement(elementId);
        Note *note = dynamic_cast<Note *>(element);
        if (!IsSchenkerSlurableNote(note)) {
            LogError("Note '%s' is not a Schenker note", elementId.c_str());
            m_editInfo.import("status", "FAILURE");
            m_editInfo.import("message", "Selected notes must be Schenker notes.");
            return false;
        }
        notes.push_back(note);
    }

    std::stable_sort(notes.begin(), notes.end(), [](Note *a, Note *b) {
        return a->GetDrawingFreeX() < b->GetDrawingFreeX();
    });

    const std::string startId = notes.front()->GetID();
    const std::string endId = notes.back()->GetID();

    Object *start = this->GetElement(startId);
    if (!start) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find start note.");
        return false;
    }

    Measure *measure = vrv_cast<Measure *>(start->GetFirstAncestor(MEASURE));
    if (!measure) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find measure for slur.");
        return false;
    }

    Object *childElement = this->PrepareInsertion(measure, "slur");
    if (!childElement) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not create slur.");
        return false;
    }

    if (!measure->AddChild(childElement)) {
        delete childElement;
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not insert slur.");
        return false;
    }

    TimePointInterface *timePointInterface = childElement->GetTimePointInterface();
    if (timePointInterface) timePointInterface->SetStartid("#" + startId);

    TimeSpanningInterface *timeSpanningInterface = childElement->GetTimeSpanningInterface();
    if (timeSpanningInterface) timeSpanningInterface->SetEndid("#" + endId);

    Slur *slur = vrv_cast<Slur *>(childElement);
    if (slur) {
        TimeSpanningInterface *ts = slur->GetTimeSpanningInterface();
        assert(ts);
        ts->SetStart(notes.front());
        ts->SetEnd(notes.back());
        slur->SetCurvedir(curvature_CURVEDIR_below);
    }

    Staff *staff = vrv_cast<Staff *>(notes.front()->GetFirstAncestor(STAFF));
    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }

    LogSchenkerStaffGeometry("K-after-schenker-slur", m_doc, staff);
    this->SetEditInfo();
    m_editInfo.import("uuid", childElement->GetID());
    m_editInfo.import("status", "OK");
    return true;
}

bool EditorToolkitShared::SetSchenkerSlurCurve(const std::string &elementId, const Point devicePoints[4])
{
    Object *element = this->GetElement(elementId);
    Slur *slur = dynamic_cast<Slur *>(element);
    if (!IsSchenkerSlurElement(slur)) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Only Schenker slurs can be edited.");
        return false;
    }
    if (!m_view) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "View is required to convert SVG/device coordinates.");
        return false;
    }

    // Exact inverse of S2 export: View::ToDeviceContext (X identity, Y =
    // pageContentHeight - y). Do not invent extra flips or scale factors.
    Point logicalPoints[4];
    for (int i = 0; i < 4; ++i) {
        logicalPoints[i] = m_view->ToLogical(devicePoints[i]);
    }

    Staff *staff = NULL;
    if (LayerElement *start = slur->GetStart()) {
        staff = vrv_cast<Staff *>(start->GetFirstAncestor(STAFF));
    }
    if (!staff) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find staff for slur curve edit.");
        return false;
    }

    if (!slur->PersistSchenkerCurve(m_doc, staff, logicalPoints)) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not persist Schenker slur geometry.");
        return false;
    }

    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }

    LogSchenkerStaffGeometry("M-after-schenker-slur-curve", m_doc, staff);
    this->SetEditInfo();
    m_editInfo.import("uuid", slur->GetID());
    m_editInfo.import("status", "OK");
    return true;
}

bool EditorToolkitShared::ResetSchenkerSlur(const std::string &elementId)
{
    Object *element = this->GetElement(elementId);
    Slur *slur = dynamic_cast<Slur *>(element);
    if (!IsSchenkerSlurElement(slur)) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Only Schenker slurs can be reset.");
        return false;
    }

    Staff *staff = NULL;
    if (LayerElement *start = slur->GetStart()) {
        staff = vrv_cast<Staff *>(start->GetFirstAncestor(STAFF));
    }

    slur->ClearSchenkerManualGeometry();

    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }

    LogSchenkerStaffGeometry("N-after-schenker-slur-reset", m_doc, staff);
    this->SetEditInfo();
    m_editInfo.import("uuid", slur->GetID());
    m_editInfo.import("status", "OK");
    return true;
}

bool EditorToolkitShared::MoveSchenkerNote(const std::string &elementId, int loc, double schenkerX)
{
    Object *element = this->GetElement(elementId);
    Note *note = dynamic_cast<Note *>(element);
    if (!IsSchenkerMovableNote(note)) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Only Schenker notes can be moved.");
        return false;
    }

    Staff *staff = vrv_cast<Staff *>(note->GetFirstAncestor(STAFF));
    Layer *layer = vrv_cast<Layer *>(note->GetFirstAncestor(LAYER));
    if (!layer) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find layer for note move.");
        return false;
    }

    note->SetLoc(loc);
    SetSchenkerX(note, schenkerX);
    if (Beam *beam = note->GetAncestorBeam()) {
        beam->ReorderByXPos();
    }
    layer->ReorderByXPos();

    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }

    LogSchenkerStaffGeometry("O-after-schenker-note-move", m_doc, staff);
    this->SetEditInfo();
    m_editInfo.import("uuid", note->GetID());
    m_editInfo.import("status", "OK");
    return true;
}

bool EditorToolkitShared::InsertSchenkerLabel(const std::string &noteId, const std::string &text)
{
    Object *element = this->GetElement(noteId);
    Note *note = dynamic_cast<Note *>(element);
    if (!note || !note->IsSchenker()) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Labels can only be attached to Schenker notes.");
        return false;
    }
    if (text.empty()) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Label text is empty.");
        return false;
    }

    Staff *staff = vrv_cast<Staff *>(note->GetFirstAncestor(STAFF));
    Measure *measure = vrv_cast<Measure *>(note->GetFirstAncestor(MEASURE));
    if (!staff || !measure) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find staff/measure for label.");
        return false;
    }

    // N2: upper/lower from actual Staff drawing order on the page — not staff@n
    // (CF-005 staves may both have n="1"; each neon-neume-line measure has one staff).
    const Staff *upperStaff = NULL;
    int maxY = INT_MIN;
    if (Page *page = m_doc->GetDrawingPage()) {
        ListOfObjects staves = page->FindAllDescendantsByType(STAFF);
        for (Object *object : staves) {
            Staff *candidate = vrv_cast<Staff *>(object);
            if (!candidate) continue;
            const int y = candidate->GetDrawingY();
            if (!upperStaff || (y > maxY)) {
                maxY = y;
                upperStaff = candidate;
            }
        }
    }
    const bool above = (staff == upperStaff);

    // Ordinary Verovio Dir — native DrawControlElementText path only.
    Object *childElement = this->PrepareInsertion(measure, "dir");
    if (!childElement) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not create label.");
        return false;
    }
    if (!measure->AddChild(childElement)) {
        delete childElement;
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not insert label.");
        return false;
    }

    Dir *dir = vrv_cast<Dir *>(childElement);
    assert(dir);
    TimePointInterface *tp = dir->GetTimePointInterface();
    assert(tp);
    tp->SetStartid("#" + note->GetID());
    tp->SetStart(note);
    dir->SetPlace(above ? STAFFREL_above : STAFFREL_below);

    Text *labelText = new Text();
    labelText->SetText(UTF8to32(text));
    if (!dir->AddChild(labelText)) {
        delete labelText;
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not store label text.");
        return false;
    }

    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }

    LogSchenkerStaffGeometry("P-after-schenker-label", m_doc, staff);
    this->SetEditInfo();
    m_editInfo.import("uuid", dir->GetID());
    m_editInfo.import("status", "OK");
    return true;
}

namespace {

double SchenkerDrawingDeltaToVu(int delta, int unit, int staffSize)
{
    if ((unit == 0) || (staffSize == 0)) return 0.0;
    return static_cast<double>(delta) * 100.0 / static_cast<double>(unit * staffSize);
}

void SchenkerSetMeasurementVu(data_MEASUREMENTSIGNED &target, double vu)
{
    target = data_MEASUREMENTSIGNED();
    target.SetVu(vu);
}

} // namespace

bool EditorToolkitShared::SetSchenkerLabelOffset(
    const std::string &elementId, const Point &fromDevice, const Point &toDevice)
{
    Object *element = this->GetElement(elementId);
    Dir *dir = dynamic_cast<Dir *>(element);
    if (!dir) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Only Dir labels can receive a visual offset.");
        return false;
    }
    LayerElement *start = dir->GetStart();
    Note *note = dynamic_cast<Note *>(start);
    if (!note || !note->IsSchenker()) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Label must be linked to a Schenker note.");
        return false;
    }
    if (!m_view) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "View is required to convert SVG/device coordinates.");
        return false;
    }

    Staff *staff = vrv_cast<Staff *>(note->GetFirstAncestor(STAFF));
    if (!staff) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find staff for label offset.");
        return false;
    }

    // Exact inverse of View::ToDeviceContext. Convert both points, then subtract.
    const Point fromLogical = m_view->ToLogical(fromDevice);
    const Point toLogical = m_view->ToLogical(toDevice);
    const int deltaX = toLogical.x - fromLogical.x;
    const int deltaY = toLogical.y - fromLogical.y;

    const int meiUnit = m_doc->GetOptions()->m_unit.GetValue();
    const int staffSize = staff->m_drawingStaffSize;
    OffsetInterface *offset = dir->GetOffsetInterface();
    assert(offset);

    double hoVu = offset->HasHo() ? offset->GetHo().GetVu() : 0.0;
    double voVu = offset->HasVo() ? offset->GetVo().GetVu() : 0.0;
    hoVu += SchenkerDrawingDeltaToVu(deltaX, meiUnit, staffSize);
    voVu += SchenkerDrawingDeltaToVu(deltaY, meiUnit, staffSize);

    data_MEASUREMENTSIGNED measurement;
    SchenkerSetMeasurementVu(measurement, hoVu);
    offset->SetHo(measurement);
    SchenkerSetMeasurementVu(measurement, voVu);
    offset->SetVo(measurement);

    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }

    LogSchenkerStaffGeometry("Q-after-schenker-label-offset", m_doc, staff);
    this->SetEditInfo();
    m_editInfo.import("uuid", dir->GetID());
    m_editInfo.import("status", "OK");
    return true;
}

bool EditorToolkitShared::SetSchenkerSlurBezier(const std::string &elementId, const Point points[4])
{
    Object *element = this->GetElement(elementId);
    Slur *slur = dynamic_cast<Slur *>(element);
    if (!IsSchenkerSlurElement(slur)) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Only Schenker slurs can be edited.");
        return false;
    }

    m_editInfo.import("status", "FAILURE");
    m_editInfo.import("message", "The retired absolute slurBezier format is no longer supported.");
    return false;
}

bool EditorToolkitShared::FlipSchenker(const std::string &elementId)
{
    Object *element = this->GetElement(elementId);
    if (!element) {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find element to flip.");
        return false;
    }

    Staff *staff = vrv_cast<Staff *>(element->GetFirstAncestor(STAFF));

    if (element->Is(NOTE)) {
        Note *note = dynamic_cast<Note *>(element);
        if (!IsSchenkerStemmedNote(note)) {
            m_editInfo.import("status", "FAILURE");
            m_editInfo.import("message", "Only unbeamed Schenker stemmed notes can be flipped.");
            return false;
        }
        if (!staff) {
            m_editInfo.import("status", "FAILURE");
            m_editInfo.import("message", "Could not find staff for note flip.");
            return false;
        }
        const data_STEMDIRECTION current = ResolveSchenkerStemDir(note, m_doc, staff);
        const data_STEMDIRECTION flipped
            = (current == STEMDIRECTION_up) ? STEMDIRECTION_down : STEMDIRECTION_up;
        ApplyStemDirToNote(note, flipped);
        LogSchenkerStaffGeometry("I-after-schenker-flip-note", m_doc, staff);
    }
    else if (element->Is(BEAM)) {
        Beam *beam = dynamic_cast<Beam *>(element);
        if (!IsSchenkerBeamElement(beam)) {
            m_editInfo.import("status", "FAILURE");
            m_editInfo.import("message", "Only Schenker beams can be flipped.");
            return false;
        }
        if (!staff) {
            m_editInfo.import("status", "FAILURE");
            m_editInfo.import("message", "Could not find staff for beam flip.");
            return false;
        }
        const data_BEAMPLACE current = ResolveSchenkerBeamPlace(beam, m_doc, staff);
        const data_BEAMPLACE flipped
            = (current == BEAMPLACE_above) ? BEAMPLACE_below : BEAMPLACE_above;
        const data_STEMDIRECTION stemDir
            = (flipped == BEAMPLACE_above) ? STEMDIRECTION_up : STEMDIRECTION_down;
        beam->SetPlace(flipped);
        ListOfObjects notes = beam->FindAllDescendantsByType(NOTE);
        for (Object *object : notes) {
            Note *note = vrv_cast<Note *>(object);
            if (note) ApplyStemDirToNote(note, stemDir);
        }
        LogSchenkerStaffGeometry("J-after-schenker-flip-beam", m_doc, staff);
        m_editInfo.import("uuid", beam->GetID());
    }
    else {
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Flip applies to Schenker notes or beams only.");
        return false;
    }

    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }
    this->SetEditInfo();
    if (!m_editInfo.has<jsonxx::String>("uuid")) {
        m_editInfo.import("uuid", elementId);
    }
    m_editInfo.import("status", "OK");
    return true;
}

void EditorToolkitShared::CollectReferringObjects(
    const Object *element, std::set<std::string> &objectsToDelete, SetOfConstObjects &visited)
{
    assert(element);

    if (visited.find(element) != visited.end()) return;
    visited.insert(element);

    // First check all children
    for (int i = 0; i < element->GetChildCount(); ++i) {
        const Object *child = element->GetChild(i);
        if (!child) continue;

        CollectReferringObjects(child, objectsToDelete, visited);
    }

    // Then find objects referring to this object
    ListOfObjectAttNamePairs referringObjects;
    FindAllReferringObjectsFunctor findAllReferringObjects(element, &referringObjects);
    m_doc->Process(findAllReferringObjects);

    for (ListOfObjectAttNamePairs::iterator it = referringObjects.begin(); it != referringObjects.end(); ++it) {
        const Object *referringObject = it->first;

        if (referringObject == NULL) continue;
        if (referringObject == element) continue;

        objectsToDelete.insert(referringObject->GetID());

        CollectReferringObjects(referringObject, objectsToDelete, visited);
    }
}

bool EditorToolkitShared::Drag(std::string &elementId, int x, int y)
{
    Object *element = this->GetChainedElement(elementId);
    if (!element) return false;

    // For elements whose y-position corresponds to a certain pitch
    if (element->HasInterface(INTERFACE_PITCH)) {
        Layer *layer = vrv_cast<Layer *>(element->GetFirstAncestor(LAYER));
        if (!layer) return false;
        int oct;
        data_PITCHNAME pname
            = (data_PITCHNAME)m_view->CalculatePitchCode(layer, m_view->ToLogicalY(y), element->GetDrawingX(), &oct);
        element->GetPitchInterface()->SetPname(pname);
        element->GetPitchInterface()->SetOct(oct);

        return true;
    }
    return false;
}

bool EditorToolkitShared::InsertSchenkerNote(
    const std::string &staffId, int loc, double schenkerX, int dur, bool voidHead, bool showStem)
{
    Object *target = this->GetElement(staffId);
    Staff *staff = dynamic_cast<Staff *>(target);
    if (!staff && target) {
        staff = dynamic_cast<Staff *>(target->GetFirstAncestor(STAFF));
    }
    if (!staff) {
        LogError("Could not find staff '%s' for structural note", staffId.c_str());
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find staff for structural note.");
        return false;
    }

    Layer *layer = vrv_cast<Layer *>(staff->FindDescendantByType(LAYER));
    if (!layer) {
        LogError("Could not find layer on staff '%s' for structural note", staffId.c_str());
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not find layer for structural note.");
        return false;
    }

    LogSchenkerStaffGeometry("C-before-CreateSchenkerNote", m_doc, staff);
    Note *note = EditorToolkit::CreateSchenkerNote(layer, loc, schenkerX, dur, voidHead, showStem);
    if (!note) {
        LogError("Could not create structural note");
        m_editInfo.import("status", "FAILURE");
        m_editInfo.import("message", "Could not create structural note.");
        return false;
    }
    LogSchenkerStaffGeometry("D-after-CreateSchenkerNote", m_doc, staff);

    layer->ReorderByXPos();
    if (Page *page = m_doc->GetDrawingPage()) {
        page->DeprecateLayout();
    }
    LogSchenkerStaffGeometry("E-before-renderToSVG", m_doc, staff);
    this->SetEditInfo();
    m_editInfo.import("uuid", note->GetID());
    m_editInfo.import("status", "OK");
    return true;
}

bool EditorToolkitShared::InsertControl(
    const std::string &elementName, const std::string startId, const std::string endId)
{
    Object *start = this->GetElement(startId);
    if (!start) return false;

    Measure *measure = vrv_cast<Measure *>(start->GetFirstAncestor(MEASURE));
    if (!measure) return false;

    Object *childElement = this->PrepareInsertion(measure, elementName);
    if (!childElement) return false;

    if (!measure->AddChild(childElement)) {
        delete childElement;
        return false;
    }

    TimePointInterface *timePointInterface = childElement->GetTimePointInterface();
    if (timePointInterface) timePointInterface->SetStartid("#" + startId);

    TimeSpanningInterface *timeSpanningInterface = childElement->GetTimeSpanningInterface();
    if (timeSpanningInterface && !endId.empty()) timeSpanningInterface->SetEndid("#" + endId);

    return true;
}

bool EditorToolkitShared::KeyDown(std::string &elementId, int key, bool shiftKey, bool ctrlKey)
{
    Object *element = this->GetChainedElement(elementId);
    if (!element) return false;

    // For elements whose y-position corresponds to a certain pitch
    if (element->HasInterface(INTERFACE_PITCH)) {
        PitchInterface *interface = element->GetPitchInterface();
        assert(interface);
        int step;
        switch (key) {
            case KEY_UP: step = 1; break;
            case KEY_DOWN: step = -1; break;
            default: step = 0;
        }
        interface->AdjustPitchByOffset(step);
        return true;
    }
    return false;
}

bool EditorToolkitShared::Navigate(std::string &elementId, const int &direction)
{
    static auto classIds = { CHORD, MREST, NOTE, REST };

    const bool forward = (direction == 39);

    m_chainedId = "";
    this->SetEditInfo();

    const Object *element = this->GetElement(elementId);
    if (!element) return false;

    const LayerElement *layerElement = dynamic_cast<const LayerElement *>(element);
    if (!layerElement) return true;

    const Layer *layer = vrv_cast<const Layer *>(layerElement->GetFirstAncestor(LAYER));
    if (!layer) return true;

    const LayerElement *result = layerElement;

    while (result) {
        // keycode left
        result = (forward) ? layer->GetNextInLayer(result) : layer->GetPreviousInLayer(result);

        if (!result || (layerElement->GetAlignment() == result->GetAlignment())) continue;

        if (result->Is(classIds)) break;
    }

    if (!result) {
        ClassIdsComparison matches(classIds);
        if (forward) {
            const Staff *staff = vrv_cast<const Staff *>(layer->GetFirstAncestor(STAFF));
            assert(staff);
            const Measure *measure = vrv_cast<const Measure *>(staff->GetFirstAncestor(MEASURE));
            assert(measure);
            const System *system = vrv_cast<const System *>(measure->GetFirstAncestor(SYSTEM));
            assert(system);
            const Measure *nextMeasure = vrv_cast<const Measure *>(system->GetNext(measure, MEASURE));
            if (!nextMeasure) {
                const Page *page = vrv_cast<const Page *>(system->GetFirstAncestor(PAGE));
                assert(page);
                const System *nextSystem = vrv_cast<const System *>(page->GetNext(system, SYSTEM));
                if (!nextSystem) {
                    const Page *nextPage = vrv_cast<const Page *>(m_doc->GetPages()->GetNext(page, PAGE));
                    if (!nextPage) return true;
                    nextSystem = vrv_cast<const System *>(nextPage->GetFirst(SYSTEM));
                    if (!nextSystem) return true;
                }
                nextMeasure = vrv_cast<const Measure *>(nextSystem->GetFirst(MEASURE));
                if (!nextMeasure) return true;
            }
            AttNIntegerComparison staffNComparison(STAFF, staff->GetN());
            const Staff *nextStaff
                = vrv_cast<const Staff *>(nextMeasure->FindDescendantByComparison(&staffNComparison));
            if (!nextStaff) return true;
            AttNIntegerComparison layerNComparison(LAYER, layer->GetN());
            layer = vrv_cast<const Layer *>(nextStaff->FindDescendantByComparison(&layerNComparison));
            if (!layer) return true;
            result = vrv_cast<const LayerElement *>(layer->FindDescendantByComparison(&matches));
        }
        else {
            const Staff *staff = vrv_cast<const Staff *>(layer->GetFirstAncestor(STAFF));
            assert(staff);
            const Measure *measure = vrv_cast<const Measure *>(staff->GetFirstAncestor(MEASURE));
            assert(measure);
            const System *system = vrv_cast<const System *>(measure->GetFirstAncestor(SYSTEM));
            assert(system);
            const Measure *previousMeasure = vrv_cast<const Measure *>(system->GetPrevious(measure, MEASURE));
            if (!previousMeasure) {
                const Page *page = vrv_cast<const Page *>(system->GetFirstAncestor(PAGE));
                assert(page);
                const System *previousSystem = vrv_cast<const System *>(page->GetPrevious(system, SYSTEM));
                if (!previousSystem) {
                    const Page *previousPage = vrv_cast<const Page *>(m_doc->GetPages()->GetPrevious(page, PAGE));
                    if (!previousPage) return true;

                    previousSystem = vrv_cast<const System *>(previousPage->GetLast(SYSTEM));
                    if (!previousSystem) return true;
                }
                previousMeasure = vrv_cast<const Measure *>(previousSystem->GetLast(MEASURE));
                if (!previousMeasure) return true;
            }
            AttNIntegerComparison staffNComparison(STAFF, staff->GetN());
            const Staff *previousStaff
                = vrv_cast<const Staff *>(previousMeasure->FindDescendantByComparison(&staffNComparison));
            if (!previousStaff) return true;
            AttNIntegerComparison layerNComparison(LAYER, layer->GetN());
            layer = vrv_cast<const Layer *>(previousStaff->FindDescendantByComparison(&layerNComparison));
            if (!layer) return true;
            result = vrv_cast<const LayerElement *>(
                layer->FindDescendantByComparison(&matches, UNLIMITED_DEPTH, BACKWARD));
        }
    }

    if (result) {
        if (result->Is(NOTE)) {
            const Note *note = vrv_cast<const Note *>(result);
            assert(note);
            if (note->IsChordTone()) result = note->IsChordTone();
        }
        if (result->Is(CHORD)) {
            const Chord *chord = vrv_cast<const Chord *>(result);
            assert(chord);
            result = chord->GetTopNote();
        }
    }

    if (result) m_chainedId = result->GetID();

    this->SetEditInfo();
    return true;
}

bool EditorToolkitShared::Set(std::string &elementId, std::string const &attribute, std::string const &value)
{
    Object *element = this->GetChainedElement(elementId);
    if (!element) return false;

    bool success = false;
    if (element->Is(TEXT) && (attribute == "text")) {
        Text *text = vrv_cast<Text *>(element);
        assert(text);
        text->SetText(UTF8to32(value));
        success = true;
    }
    else if (AttModule::SetAnalytical(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetCmn(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetCmnornaments(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetCritapp(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetEdittrans(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetExternalsymbols(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetFacsimile(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetFigtable(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetFingering(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetGestural(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetHarmony(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetHeader(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetMei(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetMensural(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetMidi(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetNeumes(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetPagebased(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetPerformance(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetShared(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetStringtab(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetUsersymbols(element, attribute, value)) {
        success = true;
    }
    else if (AttModule::SetVisual(element, attribute, value)) {
        success = true;
    }

    return success;
}

bool EditorToolkitShared::ContextForScores(bool editInfo)
{
    if (!m_scoreContext) {
        m_scoreContext = new EditorTreeObject(m_doc, false);
        ScoreContextFunctor scoreContextFunctor(m_scoreContext);
        m_doc->Process(scoreContextFunctor);
    }
    m_currentContext = m_scoreContext;

    if (!editInfo) return true;

    m_editInfo.reset();

    // The target object
    jsonxx::Object jsonObject;
    this->ContextForObject(m_scoreContext, jsonObject, true);

    m_editInfo = jsonObject;

    return true;
}

bool EditorToolkitShared::ContextForSections(bool editInfo)
{
    if (!m_sectionContext) {
        m_sectionContext = new EditorTreeObject(m_doc, false);
        SectionContextFunctor sectionContextFunctor(m_sectionContext);
        m_doc->Process(sectionContextFunctor);
    }
    m_currentContext = m_sectionContext;

    if (!editInfo) return true;

    m_editInfo.reset();

    // The target object
    jsonxx::Object jsonObject;
    this->ContextForObject(m_sectionContext, jsonObject, true);

    m_editInfo = jsonObject;

    return true;
}

void EditorToolkitShared::ClearContext()
{
    if (m_sectionContext) {
        delete m_sectionContext;
        m_sectionContext = NULL;
    }
}

bool EditorToolkitShared::ContextForElement(std::string &elementId)
{
    m_editInfo.reset();

    // Make sure we have a section tree - this also sets m_currentContext
    this->ContextForSections(false);
    assert(m_sectionContext);

    bool hasTargetID = (elementId != "");
    const Object *object = NULL;
    if (hasTargetID) {
        object = this->GetChainedElement(elementId);
    }
    // Retrieve the context from the first measure in the document
    else {
        object = m_doc->FindDescendantByType(MEASURE);
    }
    // We cannot continue without object
    if (!object || !object->GetParent()) return false;

    // Keep a pointer to the orignal object for the attributes
    const Object *originalObject = object;
    ArrayOfConstObjects siblings;
    ArrayOfConstObjects::iterator targetIt;

    const Object *contextRoot = NULL;

    // If the object parent (context root) is a sytem, this means it must be selected from the MEI section context tree
    // - and so must its siblings
    if (object->GetParent()->Is(SYSTEM)) {
        const Object *editorTreeObject = m_sectionContext->FindDescendantByID(object->GetID());
        if (!editorTreeObject) {
            return false;
        }
        // If the object is a milestone, the we must look for it in the editor tree
        if (object->IsMilestoneElement()) object = editorTreeObject;
        contextRoot = editorTreeObject->GetParent();
        siblings = this->GetScoreBasedChildrenFor(contextRoot);
        targetIt = std::find(siblings.begin(), siblings.end(), object);
        // It is not found in the siblings, something is wrong
        if (targetIt == siblings.end()) return false;
    }
    else {
        contextRoot = object->GetParent();
        siblings = object->GetParent()->GetChildren();
        targetIt = std::find(siblings.begin(), siblings.end(), object);
        // This should not happen
        if (targetIt == siblings.end()) return false;
    }
    assert(contextRoot);

    ArrayOfConstObjects previousSiblings;
    if (targetIt != siblings.begin()) std::copy(siblings.begin(), targetIt, std::back_inserter(previousSiblings));

    ArrayOfConstObjects followingSiblings;
    if (targetIt != siblings.end())
        std::copy(std::next(targetIt), siblings.end(), std::back_inserter(followingSiblings));

    ArrayOfConstObjects ancestors;
    // Reserved size for optimizing loop filling
    ancestors.reserve(10);
    jsonxx::Array jsonAncestors;

    // Look for ancestors starting from the object parent
    const Object *current = object;
    while (current->GetParent()) {
        if (current->GetParent()->Is(SYSTEM)) {
            // Switch to the MEI sectionContext tree
            current = m_sectionContext->FindDescendantByID(current->GetID());
            if (!current || !current->GetParent()) return false;
        }
        // For non-measured music, skip the parent measure object
        if (current->GetParent()->Is(MEASURE)) {
            const Measure *measure = vrv_cast<const Measure *>(current->GetParent());
            assert(measure);
            if (!measure->IsMeasuredMusic()) {
                current = current->GetParent();
                continue;
            }
        }
        // Top element in the score subtree
        if (current->GetParent()->Is(SCORE)) break;
        current = current->GetParent();
        ancestors.push_back(current);
    }
    this->ContextForObjects(ancestors, jsonAncestors);
    m_editInfo << "ancestors" << jsonAncestors;

    jsonxx::Object jsonContextRoot;
    this->ContextForObject(contextRoot, jsonContextRoot);
    jsonxx::Array jsonContext;

    // Preceeding siblings
    jsonxx::Array elements;
    this->ContextForObjects(previousSiblings, elements);
    jsonContext << elements;

    // The target object
    jsonxx::Object jsonObject;
    this->ContextForObject(object, jsonObject);
    // Include its children, but only if we specified a target ID
    if (hasTargetID) {
        jsonxx::Array jsonObjectChildren;
        ArrayOfConstObjects objectChildren;
        if (dynamic_cast<const EditorTreeObject *>(object)) {
            objectChildren = this->GetScoreBasedChildrenFor(object);
        }
        else {
            objectChildren = object->GetChildren();
        }
        this->ContextForObjects(objectChildren, jsonObjectChildren);
        if (!jsonObjectChildren.empty()) jsonObject << "children" << jsonObjectChildren;
    }
    // Add it to the list
    jsonContext << jsonObject;

    // Following siblings
    this->ContextForObjects(followingSiblings, elements);
    jsonContext << elements;

    // Add all children of to context (include target and surrounding siblings)
    jsonContextRoot << "children" << jsonContext;
    m_editInfo << "context" << jsonContextRoot;

    // Stop here without targetID, but still add empty objects or arrays to the info
    if (!hasTargetID) {
        m_editInfo << "object" << jsonxx::Object();
        m_editInfo << "referringElements" << jsonxx::Array();
        m_editInfo << "referencedElements" << jsonxx::Array();
        return true;
    }

    // Inlude all attributes
    ArrayOfStrAttr attributes;
    originalObject->GetAttributes(&attributes);
    jsonxx::Object jsonAttributes;
    for (const auto &attribute : attributes) {
        jsonAttributes << attribute.first << attribute.second;
    }
    jsonObject << "attributes" << jsonAttributes;
    std::string textStr;
    if (!dynamic_cast<const EditorTreeObject *>(object) && object->Is(TEXT)) {
        const Text *text = vrv_cast<const Text *>(object);
        assert(text);
        jsonObject << "text" << UTF32to8(text->GetText());
    }
    m_editInfo << "object" << jsonObject;

    // Find referring objects
    ListOfObjectAttNamePairs referringObjects;
    FindAllReferringObjectsFunctor findAllReferringObjects(object, &referringObjects);
    m_doc->Process(findAllReferringObjects);
    this->ContextForReferences(referringObjects, elements);
    m_editInfo << "referringElements" << elements;

    // Find referenced objects
    ListOfObjectAttNamePairs referencedObjects;
    FindAllReferencedObjectsFunctor findAllReferencedObjects(NULL, &referencedObjects);
    object->Process(findAllReferencedObjects, 0);
    this->ContextForReferences(referencedObjects, elements);
    m_editInfo << "referencedElements" << elements;

    return true;
}

void EditorToolkitShared::ContextForObject(const Object *object, jsonxx::Object &element, bool recursive)
{
    element << "element" << object->GetClassName();
    element << "id" << object->GetID();
    jsonxx::Object attributes;
    if (object->HasAttClass(ATT_NINTEGER)) {
        const AttNInteger *att = dynamic_cast<const AttNInteger *>(object);
        assert(att);
        attributes << "n" << att->GetN();
    }
    if (object->HasAttClass(ATT_NNUMBERLIKE)) {
        const AttNNumberLike *att = dynamic_cast<const AttNNumberLike *>(object);
        assert(att);
        attributes << "n" << att->GetN();
    }
    if (!attributes.empty()) {
        element << "attributes" << attributes;
    }

    ArrayOfConstObjects children;
    // First check that this is an EditorTreeObject
    if (dynamic_cast<const EditorTreeObject *>(object)) {
        children = this->GetScoreBasedChildrenFor(object);
    }
    else {
        children = object->GetChildren();
    }
    // Remove children that are added as element parts (never exist in EditorTreeObject)
    children.erase(std::remove_if(children.begin(), children.end(),
                       [](const Object *item) { return item->Is({ DOTS, FLAG, STEM, TUPLET_NUM, TUPLET_BRACKET }); }),
        children.end());

    if (children.size() > 0) {
        // If we do not call it recusrively, still include an empty array
        jsonxx::Array jsonChildren;
        if (recursive) {
            for (auto child : children) {
                jsonxx::Object jsonChild;
                this->ContextForObject(child, jsonChild, true);
                jsonChildren << jsonChild;
            }
        }
        element << "children" << jsonChildren;
    }
    else {
        element << "isLeaf" << true;
    }
}

void EditorToolkitShared::ContextForObjects(const ArrayOfConstObjects &objects, jsonxx::Array &elements)
{
    elements.reset();

    for (const Object *object : objects) {
        if (object->Is(MNUM)) {
            const MNum *mNum = vrv_cast<const MNum *>(object);
            assert(mNum);
            if (mNum->IsGenerated()) continue;
        }
        if (object->IsAttribute()) continue;
        if (object->Is({ DOTS, FLAG, STEM, TUPLET_NUM, TUPLET_BRACKET })) continue;

        jsonxx::Object element;
        this->ContextForObject(object, element);
        elements << element;
    }
}

void EditorToolkitShared::ContextForReferences(
    const ListOfObjectAttNamePairs &objectAttNames, jsonxx::Array &references)
{
    references.reset();

    for (auto &objectAttName : objectAttNames) {
        jsonxx::Object element;
        this->ContextForObject(objectAttName.first, element);
        element << "referenceAttribute" << objectAttName.second;
        references << element;
    }
}

ArrayOfConstObjects EditorToolkitShared::GetScoreBasedChildrenFor(const Object *object)
{
    // m_currentContext is set by ContextForScores or ContextForSections
    assert(m_currentContext);
    const EditorTreeObject *editorTreeObject = (m_currentContext->GetID() == object->GetID())
        ? vrv_cast<const EditorTreeObject *>(object)
        : vrv_cast<const EditorTreeObject *>(m_currentContext->FindDescendantByID(object->GetID()));
    if (!editorTreeObject) {
        return ArrayOfConstObjects();
    }
    return editorTreeObject->GetChildObjects();
}

bool EditorToolkitShared::GetScoreDef()
{
    m_editInfo.reset();

    MEIOutputExtended output(m_doc);

    m_editInfo = output.ExportScoreDef();

    return true;
}

bool EditorToolkitShared::SetScoreDef(const std::string scoreDef)
{
    return true;
}

//----------------------------------------------------------------------------
// EditorTreeObject
//----------------------------------------------------------------------------

EditorTreeObject::EditorTreeObject(const Object *object, bool ownChildren)
    : Object(object->GetClassId()), VisibilityDrawingInterface()
{
    this->Reset();

    this->SetID(object->GetID());
    m_className = object->GetClassName();
    if (this->IsEditorialElement() || this->Is(MDIV) || this->IsSystemElement()) {
        const VisibilityDrawingInterface *interface = object->GetVisibilityDrawingInterface();
        assert(interface);
        //  If we keep them hidden, then other functors will no process them.
        this->SetVisibility(interface->IsHidden() ? Hidden : Visible);
        // this->SetVisibility(Visible);
    }
    m_object = (ownChildren) ? object : NULL;
}

void EditorTreeObject::Reset()
{
    Object::Reset();
    VisibilityDrawingInterface::Reset();
}

ArrayOfConstObjects EditorTreeObject::GetChildObjects() const
{
    ArrayOfConstObjects childObjects;
    childObjects.reserve(this->GetChildCount());
    for (auto child : this->GetChildren()) {
        const EditorTreeObject *editorTreeChild = vrv_cast<const EditorTreeObject *>(child);
        // For non-measured music, get the measure object children
        if (editorTreeChild->Is(MEASURE) && editorTreeChild->m_object) {
            const Measure *measure = vrv_cast<const Measure *>(editorTreeChild->m_object);
            assert(measure);
            if (!measure->IsMeasuredMusic()) {
                ArrayOfConstObjects measureChildren = measure->GetChildren();
                std::copy(measureChildren.begin(), measureChildren.end(), std::back_inserter(childObjects));
                return childObjects;
            }
        }
        childObjects.push_back((editorTreeChild->m_object ? editorTreeChild->m_object : editorTreeChild));
    }

    return childObjects;
}

#endif /* NO_EDIT_SUPPORT */

} // namespace vrv
