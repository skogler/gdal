/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRUnionLayer class
 * Author:   Even Rouault, even dot rouault at spatialys.com
 *
 ******************************************************************************
 * Copyright (c) 2012-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef DOXYGEN_SKIP

#include "ogrunionlayer.h"
#include "ogrwarpedlayer.h"
#include "ogr_p.h"

/************************************************************************/
/*                      OGRUnionLayerGeomFieldDefn()                    */
/************************************************************************/

OGRUnionLayerGeomFieldDefn::OGRUnionLayerGeomFieldDefn(const char *pszNameIn,
                                                       OGRwkbGeometryType eType)
    : OGRGeomFieldDefn(pszNameIn, eType)
{
}

/************************************************************************/
/*                      OGRUnionLayerGeomFieldDefn()                    */
/************************************************************************/

OGRUnionLayerGeomFieldDefn::OGRUnionLayerGeomFieldDefn(
    const OGRGeomFieldDefn *poSrc)
    : OGRGeomFieldDefn(poSrc->GetNameRef(), poSrc->GetType())
{
    SetSpatialRef(poSrc->GetSpatialRef());
}

/************************************************************************/
/*                      OGRUnionLayerGeomFieldDefn()                    */
/************************************************************************/

OGRUnionLayerGeomFieldDefn::OGRUnionLayerGeomFieldDefn(
    const OGRUnionLayerGeomFieldDefn *poSrc)
    : OGRGeomFieldDefn(poSrc->GetNameRef(), poSrc->GetType()),
      bGeomTypeSet(poSrc->bGeomTypeSet), bSRSSet(poSrc->bSRSSet)
{
    SetSpatialRef(poSrc->GetSpatialRef());
    sStaticEnvelope = poSrc->sStaticEnvelope;
}

/************************************************************************/
/*                     ~OGRUnionLayerGeomFieldDefn()                    */
/************************************************************************/

OGRUnionLayerGeomFieldDefn::~OGRUnionLayerGeomFieldDefn()
{
}

/************************************************************************/
/*                          OGRUnionLayer()                             */
/************************************************************************/

// cppcheck-suppress uninitMemberVar
OGRUnionLayer::OGRUnionLayer(const char *pszName, int nSrcLayersIn,
                             OGRLayer **papoSrcLayersIn,
                             int bTakeLayerOwnership)
    : osName(pszName), nSrcLayers(nSrcLayersIn), papoSrcLayers(papoSrcLayersIn),
      bHasLayerOwnership(bTakeLayerOwnership),
      pabModifiedLayers(static_cast<int *>(CPLCalloc(sizeof(int), nSrcLayers))),
      pabCheckIfAutoWrap(static_cast<int *>(CPLCalloc(sizeof(int), nSrcLayers)))
{
    CPLAssert(nSrcLayersIn > 0);

    SetDescription(pszName);
}

/************************************************************************/
/*                         ~OGRUnionLayer()                             */
/************************************************************************/

OGRUnionLayer::~OGRUnionLayer()
{
    if (bHasLayerOwnership)
    {
        for (int i = 0; i < nSrcLayers; i++)
            delete papoSrcLayers[i];
    }
    CPLFree(papoSrcLayers);

    for (int i = 0; i < nFields; i++)
        delete papoFields[i];
    CPLFree(papoFields);
    for (int i = 0; i < nGeomFields; i++)
        delete papoGeomFields[i];
    CPLFree(papoGeomFields);

    CPLFree(pszAttributeFilter);
    CPLFree(panMap);
    CPLFree(pabModifiedLayers);
    CPLFree(pabCheckIfAutoWrap);

    if (poFeatureDefn)
        poFeatureDefn->Release();
    if (poGlobalSRS != nullptr)
        const_cast<OGRSpatialReference *>(poGlobalSRS)->Release();
}

/************************************************************************/
/*                              SetFields()                             */
/************************************************************************/

void OGRUnionLayer::SetFields(FieldUnionStrategy eFieldStrategyIn,
                              int nFieldsIn, OGRFieldDefn **papoFieldsIn,
                              int nGeomFieldsIn,
                              OGRUnionLayerGeomFieldDefn **papoGeomFieldsIn)
{
    CPLAssert(nFields == 0);
    CPLAssert(poFeatureDefn == nullptr);

    eFieldStrategy = eFieldStrategyIn;
    if (nFieldsIn)
    {
        nFields = nFieldsIn;
        papoFields = static_cast<OGRFieldDefn **>(
            CPLMalloc(nFields * sizeof(OGRFieldDefn *)));
        for (int i = 0; i < nFields; i++)
            papoFields[i] = new OGRFieldDefn(papoFieldsIn[i]);
    }
    nGeomFields = nGeomFieldsIn;
    if (nGeomFields > 0)
    {
        papoGeomFields = static_cast<OGRUnionLayerGeomFieldDefn **>(
            CPLMalloc(nGeomFields * sizeof(OGRUnionLayerGeomFieldDefn *)));
        for (int i = 0; i < nGeomFields; i++)
            papoGeomFields[i] =
                new OGRUnionLayerGeomFieldDefn(papoGeomFieldsIn[i]);
    }
}

/************************************************************************/
/*                        SetSourceLayerFieldName()                     */
/************************************************************************/

void OGRUnionLayer::SetSourceLayerFieldName(const char *pszSourceLayerFieldName)
{
    CPLAssert(poFeatureDefn == nullptr);

    CPLAssert(osSourceLayerFieldName.empty());
    if (pszSourceLayerFieldName != nullptr)
        osSourceLayerFieldName = pszSourceLayerFieldName;
}

/************************************************************************/
/*                           SetPreserveSrcFID()                        */
/************************************************************************/

void OGRUnionLayer::SetPreserveSrcFID(int bPreserveSrcFIDIn)
{
    CPLAssert(poFeatureDefn == nullptr);

    bPreserveSrcFID = bPreserveSrcFIDIn;
}

/************************************************************************/
/*                          SetFeatureCount()                           */
/************************************************************************/

void OGRUnionLayer::SetFeatureCount(int nFeatureCountIn)
{
    CPLAssert(poFeatureDefn == nullptr);

    nFeatureCount = nFeatureCountIn;
}

/************************************************************************/
/*                         MergeFieldDefn()                             */
/************************************************************************/

static void MergeFieldDefn(OGRFieldDefn *poFieldDefn,
                           const OGRFieldDefn *poSrcFieldDefn)
{
    if (poFieldDefn->GetType() != poSrcFieldDefn->GetType())
    {
        if (poSrcFieldDefn->GetType() == OFTReal &&
            (poFieldDefn->GetType() == OFTInteger ||
             poFieldDefn->GetType() == OFTInteger64))
            poFieldDefn->SetType(OFTReal);
        if (poFieldDefn->GetType() == OFTReal &&
            (poSrcFieldDefn->GetType() == OFTInteger ||
             poSrcFieldDefn->GetType() == OFTInteger64))
            poFieldDefn->SetType(OFTReal);
        else if (poSrcFieldDefn->GetType() == OFTInteger64 &&
                 poFieldDefn->GetType() == OFTInteger)
            poFieldDefn->SetType(OFTInteger64);
        else if (poFieldDefn->GetType() == OFTInteger64 &&
                 poSrcFieldDefn->GetType() == OFTInteger)
            poFieldDefn->SetType(OFTInteger64);
        else
            poFieldDefn->SetType(OFTString);
    }

    if (poFieldDefn->GetWidth() != poSrcFieldDefn->GetWidth() ||
        poFieldDefn->GetPrecision() != poSrcFieldDefn->GetPrecision())
    {
        poFieldDefn->SetWidth(0);
        poFieldDefn->SetPrecision(0);
    }
}

/************************************************************************/
/*                             GetLayerDefn()                           */
/************************************************************************/

OGRFeatureDefn *OGRUnionLayer::GetLayerDefn()
{
    if (poFeatureDefn != nullptr)
        return poFeatureDefn;

    poFeatureDefn = new OGRFeatureDefn(osName);
    poFeatureDefn->Reference();
    poFeatureDefn->SetGeomType(wkbNone);

    int iCompareFirstIndex = 0;
    if (!osSourceLayerFieldName.empty())
    {
        OGRFieldDefn oField(osSourceLayerFieldName, OFTString);
        poFeatureDefn->AddFieldDefn(&oField);
        iCompareFirstIndex = 1;
    }

    if (eFieldStrategy == FIELD_SPECIFIED)
    {
        for (int i = 0; i < nFields; i++)
            poFeatureDefn->AddFieldDefn(papoFields[i]);
        for (int i = 0; i < nGeomFields; i++)
        {
            poFeatureDefn->AddGeomFieldDefn(
                std::make_unique<OGRUnionLayerGeomFieldDefn>(
                    papoGeomFields[i]));
            OGRUnionLayerGeomFieldDefn *poGeomFieldDefn =
                cpl::down_cast<OGRUnionLayerGeomFieldDefn *>(
                    poFeatureDefn->GetGeomFieldDefn(i));

            if (poGeomFieldDefn->bGeomTypeSet == FALSE ||
                poGeomFieldDefn->bSRSSet == FALSE)
            {
                for (int iLayer = 0; iLayer < nSrcLayers; iLayer++)
                {
                    OGRFeatureDefn *poSrcFeatureDefn =
                        papoSrcLayers[iLayer]->GetLayerDefn();
                    int nIndex = poSrcFeatureDefn->GetGeomFieldIndex(
                        poGeomFieldDefn->GetNameRef());
                    if (nIndex >= 0)
                    {
                        OGRGeomFieldDefn *poSrcGeomFieldDefn =
                            poSrcFeatureDefn->GetGeomFieldDefn(nIndex);
                        if (poGeomFieldDefn->bGeomTypeSet == FALSE)
                        {
                            poGeomFieldDefn->bGeomTypeSet = TRUE;
                            poGeomFieldDefn->SetType(
                                poSrcGeomFieldDefn->GetType());
                        }
                        if (poGeomFieldDefn->bSRSSet == FALSE)
                        {
                            poGeomFieldDefn->bSRSSet = TRUE;
                            poGeomFieldDefn->SetSpatialRef(
                                poSrcGeomFieldDefn->GetSpatialRef());
                            if (i == 0 && poGlobalSRS == nullptr)
                            {
                                poGlobalSRS =
                                    poSrcGeomFieldDefn->GetSpatialRef();
                                if (poGlobalSRS != nullptr)
                                    const_cast<OGRSpatialReference *>(
                                        poGlobalSRS)
                                        ->Reference();
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
    else if (eFieldStrategy == FIELD_FROM_FIRST_LAYER)
    {
        const OGRFeatureDefn *poSrcFeatureDefn =
            papoSrcLayers[0]->GetLayerDefn();
        const int nSrcFieldCount = poSrcFeatureDefn->GetFieldCount();
        for (int i = 0; i < nSrcFieldCount; i++)
            poFeatureDefn->AddFieldDefn(poSrcFeatureDefn->GetFieldDefn(i));
        for (int i = 0;
             nGeomFields != -1 && i < poSrcFeatureDefn->GetGeomFieldCount();
             i++)
        {
            const OGRGeomFieldDefn *poFldDefn =
                poSrcFeatureDefn->GetGeomFieldDefn(i);
            poFeatureDefn->AddGeomFieldDefn(
                std::make_unique<OGRUnionLayerGeomFieldDefn>(poFldDefn));
        }
    }
    else if (eFieldStrategy == FIELD_UNION_ALL_LAYERS)
    {
        if (nGeomFields == 1)
        {
            poFeatureDefn->AddGeomFieldDefn(
                std::make_unique<OGRUnionLayerGeomFieldDefn>(
                    papoGeomFields[0]));
        }

        int nDstFieldCount = 0;
        std::map<std::string, int> oMapDstFieldNameToIdx;

        for (int iLayer = 0; iLayer < nSrcLayers; iLayer++)
        {
            const OGRFeatureDefn *poSrcFeatureDefn =
                papoSrcLayers[iLayer]->GetLayerDefn();

            /* Add any field that is found in the source layers */
            const int nSrcFieldCount = poSrcFeatureDefn->GetFieldCount();
            for (int i = 0; i < nSrcFieldCount; i++)
            {
                const OGRFieldDefn *poSrcFieldDefn =
                    poSrcFeatureDefn->GetFieldDefn(i);
                const auto oIter =
                    oMapDstFieldNameToIdx.find(poSrcFieldDefn->GetNameRef());
                const int nIndex =
                    oIter == oMapDstFieldNameToIdx.end() ? -1 : oIter->second;
                if (nIndex < 0)
                {
                    oMapDstFieldNameToIdx[poSrcFieldDefn->GetNameRef()] =
                        nDstFieldCount;
                    nDstFieldCount++;
                    poFeatureDefn->AddFieldDefn(poSrcFieldDefn);
                }
                else
                {
                    OGRFieldDefn *poFieldDefn =
                        poFeatureDefn->GetFieldDefn(nIndex);
                    MergeFieldDefn(poFieldDefn, poSrcFieldDefn);
                }
            }

            for (int i = 0;
                 nGeomFields != -1 && i < poSrcFeatureDefn->GetGeomFieldCount();
                 i++)
            {
                const OGRGeomFieldDefn *poSrcFieldDefn =
                    poSrcFeatureDefn->GetGeomFieldDefn(i);
                int nIndex = poFeatureDefn->GetGeomFieldIndex(
                    poSrcFieldDefn->GetNameRef());
                if (nIndex < 0)
                {
                    poFeatureDefn->AddGeomFieldDefn(
                        std::make_unique<OGRUnionLayerGeomFieldDefn>(
                            poSrcFieldDefn));
                    if (poFeatureDefn->GetGeomFieldCount() == 1 &&
                        nGeomFields == 0 && GetSpatialRef() != nullptr)
                    {
                        OGRUnionLayerGeomFieldDefn *poGeomFieldDefn =
                            cpl::down_cast<OGRUnionLayerGeomFieldDefn *>(
                                poFeatureDefn->GetGeomFieldDefn(0));
                        poGeomFieldDefn->bSRSSet = TRUE;
                        poGeomFieldDefn->SetSpatialRef(GetSpatialRef());
                    }
                }
                else
                {
                    if (nIndex == 0 && nGeomFields == 1)
                    {
                        OGRUnionLayerGeomFieldDefn *poGeomFieldDefn =
                            cpl::down_cast<OGRUnionLayerGeomFieldDefn *>(
                                poFeatureDefn->GetGeomFieldDefn(0));
                        if (poGeomFieldDefn->bGeomTypeSet == FALSE)
                        {
                            poGeomFieldDefn->bGeomTypeSet = TRUE;
                            poGeomFieldDefn->SetType(poSrcFieldDefn->GetType());
                        }
                        if (poGeomFieldDefn->bSRSSet == FALSE)
                        {
                            poGeomFieldDefn->bSRSSet = TRUE;
                            poGeomFieldDefn->SetSpatialRef(
                                poSrcFieldDefn->GetSpatialRef());
                        }
                    }
                    /* TODO: merge type, SRS, extent ? */
                }
            }
        }
    }
    else if (eFieldStrategy == FIELD_INTERSECTION_ALL_LAYERS)
    {
        OGRFeatureDefn *poSrcFeatureDefn = papoSrcLayers[0]->GetLayerDefn();
        for (int i = 0; i < poSrcFeatureDefn->GetFieldCount(); i++)
            poFeatureDefn->AddFieldDefn(poSrcFeatureDefn->GetFieldDefn(i));
        for (int i = 0; i < poSrcFeatureDefn->GetGeomFieldCount(); i++)
        {
            OGRGeomFieldDefn *poFldDefn = poSrcFeatureDefn->GetGeomFieldDefn(i);
            poFeatureDefn->AddGeomFieldDefn(
                std::make_unique<OGRUnionLayerGeomFieldDefn>(poFldDefn));
        }

        /* Remove any field that is not found in the source layers */
        for (int iLayer = 1; iLayer < nSrcLayers; iLayer++)
        {
            OGRFeatureDefn *l_poSrcFeatureDefn =
                papoSrcLayers[iLayer]->GetLayerDefn();
            for (int i = iCompareFirstIndex; i < poFeatureDefn->GetFieldCount();
                 // No increment.
            )
            {
                OGRFieldDefn *poFieldDefn = poFeatureDefn->GetFieldDefn(i);
                int nSrcIndex = l_poSrcFeatureDefn->GetFieldIndex(
                    poFieldDefn->GetNameRef());
                if (nSrcIndex < 0)
                {
                    poFeatureDefn->DeleteFieldDefn(i);
                }
                else
                {
                    OGRFieldDefn *poSrcFieldDefn =
                        l_poSrcFeatureDefn->GetFieldDefn(nSrcIndex);
                    MergeFieldDefn(poFieldDefn, poSrcFieldDefn);

                    i++;
                }
            }
            for (int i = 0; i < poFeatureDefn->GetGeomFieldCount();
                 // No increment.
            )
            {
                OGRGeomFieldDefn *poFieldDefn =
                    poFeatureDefn->GetGeomFieldDefn(i);
                int nSrcIndex = l_poSrcFeatureDefn->GetGeomFieldIndex(
                    poFieldDefn->GetNameRef());
                if (nSrcIndex < 0)
                {
                    poFeatureDefn->DeleteGeomFieldDefn(i);
                }
                else
                {
                    /* TODO: merge type, SRS, extent ? */

                    i++;
                }
            }
        }
    }

    return poFeatureDefn;
}

/************************************************************************/
/*                             GetGeomType()                            */
/************************************************************************/

OGRwkbGeometryType OGRUnionLayer::GetGeomType()
{
    if (nGeomFields < 0)
        return wkbNone;
    if (nGeomFields >= 1 && papoGeomFields[0]->bGeomTypeSet)
    {
        return papoGeomFields[0]->GetType();
    }

    return OGRLayer::GetGeomType();
}

/************************************************************************/
/*                    SetSpatialFilterToSourceLayer()                   */
/************************************************************************/

void OGRUnionLayer::SetSpatialFilterToSourceLayer(OGRLayer *poSrcLayer)
{
    if (m_iGeomFieldFilter >= 0 &&
        m_iGeomFieldFilter < GetLayerDefn()->GetGeomFieldCount())
    {
        int iSrcGeomField = poSrcLayer->GetLayerDefn()->GetGeomFieldIndex(
            GetLayerDefn()->GetGeomFieldDefn(m_iGeomFieldFilter)->GetNameRef());
        if (iSrcGeomField >= 0)
        {
            poSrcLayer->SetSpatialFilter(iSrcGeomField, m_poFilterGeom);
        }
        else
        {
            poSrcLayer->SetSpatialFilter(nullptr);
        }
    }
    else
    {
        poSrcLayer->SetSpatialFilter(nullptr);
    }
}

/************************************************************************/
/*                        ConfigureActiveLayer()                        */
/************************************************************************/

void OGRUnionLayer::ConfigureActiveLayer()
{
    AutoWarpLayerIfNecessary(iCurLayer);
    ApplyAttributeFilterToSrcLayer(iCurLayer);
    SetSpatialFilterToSourceLayer(papoSrcLayers[iCurLayer]);
    papoSrcLayers[iCurLayer]->ResetReading();

    /* Establish map */
    GetLayerDefn();
    const OGRFeatureDefn *poSrcFeatureDefn =
        papoSrcLayers[iCurLayer]->GetLayerDefn();
    const int nSrcFieldCount = poSrcFeatureDefn->GetFieldCount();
    const int nDstFieldCount = poFeatureDefn->GetFieldCount();

    std::map<std::string, int> oMapDstFieldNameToIdx;
    for (int i = 0; i < nDstFieldCount; i++)
    {
        const OGRFieldDefn *poDstFieldDefn = poFeatureDefn->GetFieldDefn(i);
        oMapDstFieldNameToIdx[poDstFieldDefn->GetNameRef()] = i;
    }

    CPLFree(panMap);
    panMap = static_cast<int *>(CPLMalloc(nSrcFieldCount * sizeof(int)));
    for (int i = 0; i < nSrcFieldCount; i++)
    {
        const OGRFieldDefn *poSrcFieldDefn = poSrcFeatureDefn->GetFieldDefn(i);
        if (m_aosIgnoredFields.FindString(poSrcFieldDefn->GetNameRef()) == -1)
        {
            const auto oIter =
                oMapDstFieldNameToIdx.find(poSrcFieldDefn->GetNameRef());
            panMap[i] =
                oIter == oMapDstFieldNameToIdx.end() ? -1 : oIter->second;
        }
        else
        {
            panMap[i] = -1;
        }
    }

    if (papoSrcLayers[iCurLayer]->TestCapability(OLCIgnoreFields))
    {
        CPLStringList aosFieldSrc;
        for (const char *pszFieldName : cpl::Iterate(m_aosIgnoredFields))
        {
            if (EQUAL(pszFieldName, "OGR_GEOMETRY") ||
                EQUAL(pszFieldName, "OGR_STYLE") ||
                poSrcFeatureDefn->GetFieldIndex(pszFieldName) >= 0 ||
                poSrcFeatureDefn->GetGeomFieldIndex(pszFieldName) >= 0)
            {
                aosFieldSrc.AddString(pszFieldName);
            }
        }

        std::map<std::string, int> oMapSrcFieldNameToIdx;
        for (int i = 0; i < nSrcFieldCount; i++)
        {
            const OGRFieldDefn *poSrcFieldDefn =
                poSrcFeatureDefn->GetFieldDefn(i);
            oMapSrcFieldNameToIdx[poSrcFieldDefn->GetNameRef()] = i;
        }

        /* Attribute fields */
        std::vector<bool> abSrcFieldsUsed(nSrcFieldCount);
        for (int iField = 0; iField < nDstFieldCount; iField++)
        {
            const OGRFieldDefn *poFieldDefn =
                poFeatureDefn->GetFieldDefn(iField);
            const auto oIter =
                oMapSrcFieldNameToIdx.find(poFieldDefn->GetNameRef());
            const int iSrcField =
                oIter == oMapSrcFieldNameToIdx.end() ? -1 : oIter->second;
            if (iSrcField >= 0)
                abSrcFieldsUsed[iSrcField] = true;
        }
        for (int iSrcField = 0; iSrcField < nSrcFieldCount; iSrcField++)
        {
            if (!abSrcFieldsUsed[iSrcField])
            {
                const OGRFieldDefn *poSrcDefn =
                    poSrcFeatureDefn->GetFieldDefn(iSrcField);
                aosFieldSrc.AddString(poSrcDefn->GetNameRef());
            }
        }

        /* geometry fields now */
        abSrcFieldsUsed.clear();
        abSrcFieldsUsed.resize(poSrcFeatureDefn->GetGeomFieldCount());
        for (int iField = 0; iField < poFeatureDefn->GetGeomFieldCount();
             iField++)
        {
            const OGRGeomFieldDefn *poFieldDefn =
                poFeatureDefn->GetGeomFieldDefn(iField);
            const int iSrcField =
                poSrcFeatureDefn->GetGeomFieldIndex(poFieldDefn->GetNameRef());
            if (iSrcField >= 0)
                abSrcFieldsUsed[iSrcField] = true;
        }
        for (int iSrcField = 0;
             iSrcField < poSrcFeatureDefn->GetGeomFieldCount(); iSrcField++)
        {
            if (!abSrcFieldsUsed[iSrcField])
            {
                const OGRGeomFieldDefn *poSrcDefn =
                    poSrcFeatureDefn->GetGeomFieldDefn(iSrcField);
                aosFieldSrc.AddString(poSrcDefn->GetNameRef());
            }
        }

        papoSrcLayers[iCurLayer]->SetIgnoredFields(aosFieldSrc.List());
    }
}

/************************************************************************/
/*                             ResetReading()                           */
/************************************************************************/

void OGRUnionLayer::ResetReading()
{
    iCurLayer = 0;
    ConfigureActiveLayer();
    nNextFID = 0;
}

/************************************************************************/
/*                         AutoWarpLayerIfNecessary()                   */
/************************************************************************/

void OGRUnionLayer::AutoWarpLayerIfNecessary(int iLayer)
{
    if (!pabCheckIfAutoWrap[iLayer])
    {
        pabCheckIfAutoWrap[iLayer] = TRUE;

        for (int i = 0; i < GetLayerDefn()->GetGeomFieldCount(); i++)
        {
            const OGRSpatialReference *poSRS =
                GetLayerDefn()->GetGeomFieldDefn(i)->GetSpatialRef();

            OGRFeatureDefn *poSrcFeatureDefn =
                papoSrcLayers[iLayer]->GetLayerDefn();
            int iSrcGeomField = poSrcFeatureDefn->GetGeomFieldIndex(
                GetLayerDefn()->GetGeomFieldDefn(i)->GetNameRef());
            if (iSrcGeomField >= 0)
            {
                const OGRSpatialReference *poSRS2 =
                    poSrcFeatureDefn->GetGeomFieldDefn(iSrcGeomField)
                        ->GetSpatialRef();

                if ((poSRS == nullptr && poSRS2 != nullptr) ||
                    (poSRS != nullptr && poSRS2 == nullptr))
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "SRS of geometry field '%s' layer %s not "
                             "consistent with UnionLayer SRS",
                             GetLayerDefn()->GetGeomFieldDefn(i)->GetNameRef(),
                             papoSrcLayers[iLayer]->GetName());
                }
                else if (poSRS != nullptr && poSRS2 != nullptr &&
                         poSRS != poSRS2 && !poSRS->IsSame(poSRS2))
                {
                    CPLDebug("VRT",
                             "SRS of geometry field '%s' layer %s not "
                             "consistent with UnionLayer SRS. "
                             "Trying auto warping",
                             GetLayerDefn()->GetGeomFieldDefn(i)->GetNameRef(),
                             papoSrcLayers[iLayer]->GetName());
                    OGRCoordinateTransformation *poCT =
                        OGRCreateCoordinateTransformation(poSRS2, poSRS);
                    OGRCoordinateTransformation *poReversedCT =
                        (poCT != nullptr)
                            ? OGRCreateCoordinateTransformation(poSRS, poSRS2)
                            : nullptr;
                    if (poReversedCT != nullptr)
                        papoSrcLayers[iLayer] = new OGRWarpedLayer(
                            papoSrcLayers[iLayer], iSrcGeomField, TRUE, poCT,
                            poReversedCT);
                    else
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "AutoWarpLayerIfNecessary failed to create "
                                 "poCT or poReversedCT.");
                        if (poCT != nullptr)
                            delete poCT;
                    }
                }
            }
        }
    }
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRUnionLayer::GetNextFeature()
{
    if (poFeatureDefn == nullptr)
        GetLayerDefn();
    if (iCurLayer < 0)
        ResetReading();

    if (iCurLayer == nSrcLayers)
        return nullptr;

    while (true)
    {
        OGRFeature *poSrcFeature = papoSrcLayers[iCurLayer]->GetNextFeature();
        if (poSrcFeature == nullptr)
        {
            iCurLayer++;
            if (iCurLayer < nSrcLayers)
            {
                ConfigureActiveLayer();
                continue;
            }
            else
                break;
        }

        OGRFeature *poFeature = TranslateFromSrcLayer(poSrcFeature);
        delete poSrcFeature;

        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeomFieldRef(m_iGeomFieldFilter))) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
        {
            return poFeature;
        }

        delete poFeature;
    }
    return nullptr;
}

/************************************************************************/
/*                             GetFeature()                             */
/************************************************************************/

OGRFeature *OGRUnionLayer::GetFeature(GIntBig nFeatureId)
{
    OGRFeature *poFeature = nullptr;

    if (!bPreserveSrcFID)
    {
        poFeature = OGRLayer::GetFeature(nFeatureId);
    }
    else
    {
        int iGeomFieldFilterSave = m_iGeomFieldFilter;
        OGRGeometry *poGeomSave = m_poFilterGeom;
        m_poFilterGeom = nullptr;
        SetSpatialFilter(nullptr);

        for (int i = 0; i < nSrcLayers; i++)
        {
            iCurLayer = i;
            ConfigureActiveLayer();

            OGRFeature *poSrcFeature = papoSrcLayers[i]->GetFeature(nFeatureId);
            if (poSrcFeature != nullptr)
            {
                poFeature = TranslateFromSrcLayer(poSrcFeature);
                delete poSrcFeature;

                break;
            }
        }

        SetSpatialFilter(iGeomFieldFilterSave, poGeomSave);
        delete poGeomSave;

        ResetReading();
    }

    return poFeature;
}

/************************************************************************/
/*                          ICreateFeature()                             */
/************************************************************************/

OGRErr OGRUnionLayer::ICreateFeature(OGRFeature *poFeature)
{
    if (osSourceLayerFieldName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "CreateFeature() not supported when SourceLayerFieldName is "
                 "not set");
        return OGRERR_FAILURE;
    }

    if (poFeature->GetFID() != OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "CreateFeature() not supported when FID is set");
        return OGRERR_FAILURE;
    }

    if (!poFeature->IsFieldSetAndNotNull(0))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "CreateFeature() not supported when '%s' field is not set",
                 osSourceLayerFieldName.c_str());
        return OGRERR_FAILURE;
    }

    const char *pszSrcLayerName = poFeature->GetFieldAsString(0);
    for (int i = 0; i < nSrcLayers; i++)
    {
        if (strcmp(pszSrcLayerName, papoSrcLayers[i]->GetName()) == 0)
        {
            pabModifiedLayers[i] = TRUE;

            OGRFeature *poSrcFeature =
                new OGRFeature(papoSrcLayers[i]->GetLayerDefn());
            poSrcFeature->SetFrom(poFeature, TRUE);
            OGRErr eErr = papoSrcLayers[i]->CreateFeature(poSrcFeature);
            if (eErr == OGRERR_NONE)
                poFeature->SetFID(poSrcFeature->GetFID());
            delete poSrcFeature;
            return eErr;
        }
    }

    CPLError(CE_Failure, CPLE_NotSupported,
             "CreateFeature() not supported : '%s' source layer does not exist",
             pszSrcLayerName);
    return OGRERR_FAILURE;
}

/************************************************************************/
/*                             ISetFeature()                             */
/************************************************************************/

OGRErr OGRUnionLayer::ISetFeature(OGRFeature *poFeature)
{
    if (!bPreserveSrcFID)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "SetFeature() not supported when PreserveSrcFID is OFF");
        return OGRERR_FAILURE;
    }

    if (osSourceLayerFieldName.empty())
    {
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "SetFeature() not supported when SourceLayerFieldName is not set");
        return OGRERR_FAILURE;
    }

    if (poFeature->GetFID() == OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "SetFeature() not supported when FID is not set");
        return OGRERR_FAILURE;
    }

    if (!poFeature->IsFieldSetAndNotNull(0))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "SetFeature() not supported when '%s' field is not set",
                 osSourceLayerFieldName.c_str());
        return OGRERR_FAILURE;
    }

    const char *pszSrcLayerName = poFeature->GetFieldAsString(0);
    for (int i = 0; i < nSrcLayers; i++)
    {
        if (strcmp(pszSrcLayerName, papoSrcLayers[i]->GetName()) == 0)
        {
            pabModifiedLayers[i] = TRUE;

            OGRFeature *poSrcFeature =
                new OGRFeature(papoSrcLayers[i]->GetLayerDefn());
            poSrcFeature->SetFrom(poFeature, TRUE);
            poSrcFeature->SetFID(poFeature->GetFID());
            OGRErr eErr = papoSrcLayers[i]->SetFeature(poSrcFeature);
            delete poSrcFeature;
            return eErr;
        }
    }

    CPLError(CE_Failure, CPLE_NotSupported,
             "SetFeature() not supported : '%s' source layer does not exist",
             pszSrcLayerName);
    return OGRERR_FAILURE;
}

/************************************************************************/
/*                          IUpsertFeature()                            */
/************************************************************************/

OGRErr OGRUnionLayer::IUpsertFeature(OGRFeature *poFeature)
{
    if (GetFeature(poFeature->GetFID()))
    {
        return ISetFeature(poFeature);
    }
    else
    {
        return ICreateFeature(poFeature);
    }
}

/************************************************************************/
/*                           IUpdateFeature()                           */
/************************************************************************/

OGRErr OGRUnionLayer::IUpdateFeature(OGRFeature *poFeature,
                                     int nUpdatedFieldsCount,
                                     const int *panUpdatedFieldsIdx,
                                     int nUpdatedGeomFieldsCount,
                                     const int *panUpdatedGeomFieldsIdx,
                                     bool bUpdateStyleString)
{
    if (!bPreserveSrcFID)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "UpdateFeature() not supported when PreserveSrcFID is OFF");
        return OGRERR_FAILURE;
    }

    if (osSourceLayerFieldName.empty())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "UpdateFeature() not supported when SourceLayerFieldName is "
                 "not set");
        return OGRERR_FAILURE;
    }

    if (poFeature->GetFID() == OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "UpdateFeature() not supported when FID is not set");
        return OGRERR_FAILURE;
    }

    if (!poFeature->IsFieldSetAndNotNull(0))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "UpdateFeature() not supported when '%s' field is not set",
                 osSourceLayerFieldName.c_str());
        return OGRERR_FAILURE;
    }

    const char *pszSrcLayerName = poFeature->GetFieldAsString(0);
    for (int i = 0; i < nSrcLayers; i++)
    {
        if (strcmp(pszSrcLayerName, papoSrcLayers[i]->GetName()) == 0)
        {
            pabModifiedLayers[i] = TRUE;

            const auto poSrcLayerDefn = papoSrcLayers[i]->GetLayerDefn();
            OGRFeature *poSrcFeature = new OGRFeature(poSrcLayerDefn);
            poSrcFeature->SetFrom(poFeature, TRUE);
            poSrcFeature->SetFID(poFeature->GetFID());

            // We could potentially have a pre-computed map from indices in
            // poLayerDefn to indices in poSrcLayerDefn
            std::vector<int> anSrcUpdatedFieldIdx;
            const auto poLayerDefn = GetLayerDefn();
            for (int j = 0; j < nUpdatedFieldsCount; ++j)
            {
                if (panUpdatedFieldsIdx[j] != 0)
                {
                    const int nNewIdx = poSrcLayerDefn->GetFieldIndex(
                        poLayerDefn->GetFieldDefn(panUpdatedFieldsIdx[j])
                            ->GetNameRef());
                    if (nNewIdx >= 0)
                    {
                        anSrcUpdatedFieldIdx.push_back(nNewIdx);
                    }
                }
            }
            std::vector<int> anSrcUpdatedGeomFieldIdx;
            for (int j = 0; j < nUpdatedGeomFieldsCount; ++j)
            {
                if (panUpdatedGeomFieldsIdx[j] != 0)
                {
                    const int nNewIdx = poSrcLayerDefn->GetGeomFieldIndex(
                        poLayerDefn
                            ->GetGeomFieldDefn(panUpdatedGeomFieldsIdx[j])
                            ->GetNameRef());
                    if (nNewIdx >= 0)
                    {
                        anSrcUpdatedGeomFieldIdx.push_back(nNewIdx);
                    }
                }
            }

            OGRErr eErr = papoSrcLayers[i]->UpdateFeature(
                poSrcFeature, static_cast<int>(anSrcUpdatedFieldIdx.size()),
                anSrcUpdatedFieldIdx.data(),
                static_cast<int>(anSrcUpdatedGeomFieldIdx.size()),
                anSrcUpdatedGeomFieldIdx.data(), bUpdateStyleString);
            delete poSrcFeature;
            return eErr;
        }
    }

    CPLError(CE_Failure, CPLE_NotSupported,
             "UpdateFeature() not supported : '%s' source layer does not exist",
             pszSrcLayerName);
    return OGRERR_FAILURE;
}

/************************************************************************/
/*                           GetSpatialRef()                            */
/************************************************************************/

OGRSpatialReference *OGRUnionLayer::GetSpatialRef()
{
    if (nGeomFields < 0)
        return nullptr;
    if (nGeomFields >= 1 && papoGeomFields[0]->bSRSSet)
        return const_cast<OGRSpatialReference *>(
            papoGeomFields[0]->GetSpatialRef());

    if (poGlobalSRS == nullptr)
    {
        poGlobalSRS = papoSrcLayers[0]->GetSpatialRef();
        if (poGlobalSRS != nullptr)
            const_cast<OGRSpatialReference *>(poGlobalSRS)->Reference();
    }
    return const_cast<OGRSpatialReference *>(poGlobalSRS);
}

/************************************************************************/
/*                      GetAttrFilterPassThroughValue()                 */
/************************************************************************/

int OGRUnionLayer::GetAttrFilterPassThroughValue()
{
    if (m_poAttrQuery == nullptr)
        return TRUE;

    if (bAttrFilterPassThroughValue >= 0)
        return bAttrFilterPassThroughValue;

    char **papszUsedFields = m_poAttrQuery->GetUsedFields();
    int bRet = TRUE;

    for (int iLayer = 0; iLayer < nSrcLayers; iLayer++)
    {
        OGRFeatureDefn *poSrcFeatureDefn =
            papoSrcLayers[iLayer]->GetLayerDefn();
        char **papszIter = papszUsedFields;
        while (papszIter != nullptr && *papszIter != nullptr)
        {
            int bIsSpecial = FALSE;
            for (int i = 0; i < SPECIAL_FIELD_COUNT; i++)
            {
                if (EQUAL(*papszIter, SpecialFieldNames[i]))
                {
                    bIsSpecial = TRUE;
                    break;
                }
            }
            if (!bIsSpecial && poSrcFeatureDefn->GetFieldIndex(*papszIter) < 0)
            {
                bRet = FALSE;
                break;
            }
            papszIter++;
        }
    }

    CSLDestroy(papszUsedFields);

    bAttrFilterPassThroughValue = bRet;

    return bRet;
}

/************************************************************************/
/*                  ApplyAttributeFilterToSrcLayer()                    */
/************************************************************************/

void OGRUnionLayer::ApplyAttributeFilterToSrcLayer(int iSubLayer)
{
    CPLAssert(iSubLayer >= 0 && iSubLayer < nSrcLayers);

    if (GetAttrFilterPassThroughValue())
        papoSrcLayers[iSubLayer]->SetAttributeFilter(pszAttributeFilter);
    else
        papoSrcLayers[iSubLayer]->SetAttributeFilter(nullptr);
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/************************************************************************/

GIntBig OGRUnionLayer::GetFeatureCount(int bForce)
{
    if (nFeatureCount >= 0 && m_poFilterGeom == nullptr &&
        m_poAttrQuery == nullptr)
    {
        return nFeatureCount;
    }

    if (!GetAttrFilterPassThroughValue())
        return OGRLayer::GetFeatureCount(bForce);

    GIntBig nRet = 0;
    for (int i = 0; i < nSrcLayers; i++)
    {
        AutoWarpLayerIfNecessary(i);
        ApplyAttributeFilterToSrcLayer(i);
        SetSpatialFilterToSourceLayer(papoSrcLayers[i]);
        nRet += papoSrcLayers[i]->GetFeatureCount(bForce);
    }
    ResetReading();
    return nRet;
}

/************************************************************************/
/*                         SetAttributeFilter()                         */
/************************************************************************/

OGRErr OGRUnionLayer::SetAttributeFilter(const char *pszAttributeFilterIn)
{
    if (pszAttributeFilterIn == nullptr && pszAttributeFilter == nullptr)
        return OGRERR_NONE;
    if (pszAttributeFilterIn != nullptr && pszAttributeFilter != nullptr &&
        strcmp(pszAttributeFilterIn, pszAttributeFilter) == 0)
        return OGRERR_NONE;

    if (poFeatureDefn == nullptr)
        GetLayerDefn();

    bAttrFilterPassThroughValue = -1;

    OGRErr eErr = OGRLayer::SetAttributeFilter(pszAttributeFilterIn);
    if (eErr != OGRERR_NONE)
        return eErr;

    CPLFree(pszAttributeFilter);
    pszAttributeFilter =
        pszAttributeFilterIn ? CPLStrdup(pszAttributeFilterIn) : nullptr;

    if (iCurLayer >= 0 && iCurLayer < nSrcLayers)
        ApplyAttributeFilterToSrcLayer(iCurLayer);

    return OGRERR_NONE;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRUnionLayer::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, OLCFastFeatureCount))
    {
        if (nFeatureCount >= 0 && m_poFilterGeom == nullptr &&
            m_poAttrQuery == nullptr)
            return TRUE;

        if (!GetAttrFilterPassThroughValue())
            return FALSE;

        for (int i = 0; i < nSrcLayers; i++)
        {
            AutoWarpLayerIfNecessary(i);
            ApplyAttributeFilterToSrcLayer(i);
            SetSpatialFilterToSourceLayer(papoSrcLayers[i]);
            if (!papoSrcLayers[i]->TestCapability(pszCap))
                return FALSE;
        }
        return TRUE;
    }

    if (EQUAL(pszCap, OLCFastGetExtent))
    {
        if (nGeomFields >= 1 && papoGeomFields[0]->sStaticEnvelope.IsInit())
            return TRUE;

        for (int i = 0; i < nSrcLayers; i++)
        {
            AutoWarpLayerIfNecessary(i);
            if (!papoSrcLayers[i]->TestCapability(pszCap))
                return FALSE;
        }
        return TRUE;
    }

    if (EQUAL(pszCap, OLCFastSpatialFilter))
    {
        for (int i = 0; i < nSrcLayers; i++)
        {
            AutoWarpLayerIfNecessary(i);
            ApplyAttributeFilterToSrcLayer(i);
            if (!papoSrcLayers[i]->TestCapability(pszCap))
                return FALSE;
        }
        return TRUE;
    }

    if (EQUAL(pszCap, OLCStringsAsUTF8))
    {
        for (int i = 0; i < nSrcLayers; i++)
        {
            if (!papoSrcLayers[i]->TestCapability(pszCap))
                return FALSE;
        }
        return TRUE;
    }

    if (EQUAL(pszCap, OLCRandomRead))
    {
        if (!bPreserveSrcFID)
            return FALSE;

        for (int i = 0; i < nSrcLayers; i++)
        {
            if (!papoSrcLayers[i]->TestCapability(pszCap))
                return FALSE;
        }
        return TRUE;
    }

    if (EQUAL(pszCap, OLCRandomWrite))
    {
        if (!bPreserveSrcFID || osSourceLayerFieldName.empty())
            return FALSE;

        for (int i = 0; i < nSrcLayers; i++)
        {
            if (!papoSrcLayers[i]->TestCapability(pszCap))
                return FALSE;
        }
        return TRUE;
    }

    if (EQUAL(pszCap, OLCSequentialWrite))
    {
        if (osSourceLayerFieldName.empty())
            return FALSE;

        for (int i = 0; i < nSrcLayers; i++)
        {
            if (!papoSrcLayers[i]->TestCapability(pszCap))
                return FALSE;
        }
        return TRUE;
    }

    if (EQUAL(pszCap, OLCIgnoreFields))
        return TRUE;

    if (EQUAL(pszCap, OLCCurveGeometries))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                              GetExtent()                             */
/************************************************************************/

OGRErr OGRUnionLayer::GetExtent(int iGeomField, OGREnvelope *psExtent,
                                int bForce)
{
    if (iGeomField >= 0 && iGeomField < nGeomFields &&
        papoGeomFields[iGeomField]->sStaticEnvelope.IsInit())
    {
        *psExtent = papoGeomFields[iGeomField]->sStaticEnvelope;
        return OGRERR_NONE;
    }

    if (iGeomField < 0 || iGeomField >= GetLayerDefn()->GetGeomFieldCount())
    {
        if (iGeomField != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid geometry field index : %d", iGeomField);
        }
        return OGRERR_FAILURE;
    }

    int bInit = FALSE;
    for (int i = 0; i < nSrcLayers; i++)
    {
        AutoWarpLayerIfNecessary(i);
        int iSrcGeomField = papoSrcLayers[i]->GetLayerDefn()->GetGeomFieldIndex(
            GetLayerDefn()->GetGeomFieldDefn(iGeomField)->GetNameRef());
        if (iSrcGeomField >= 0)
        {
            if (!bInit)
            {
                if (papoSrcLayers[i]->GetExtent(iSrcGeomField, psExtent,
                                                bForce) == OGRERR_NONE)
                    bInit = TRUE;
            }
            else
            {
                OGREnvelope sExtent;
                if (papoSrcLayers[i]->GetExtent(iSrcGeomField, &sExtent,
                                                bForce) == OGRERR_NONE)
                {
                    psExtent->Merge(sExtent);
                }
            }
        }
    }
    return (bInit) ? OGRERR_NONE : OGRERR_FAILURE;
}

/************************************************************************/
/*                             GetExtent()                              */
/************************************************************************/

OGRErr OGRUnionLayer::GetExtent(OGREnvelope *psExtent, int bForce)
{
    return GetExtent(0, psExtent, bForce);
}

/************************************************************************/
/*                          SetSpatialFilter()                          */
/************************************************************************/

void OGRUnionLayer::SetSpatialFilter(OGRGeometry *poGeomIn)
{
    SetSpatialFilter(0, poGeomIn);
}

/************************************************************************/
/*                         SetSpatialFilter()                           */
/************************************************************************/

void OGRUnionLayer::SetSpatialFilter(int iGeomField, OGRGeometry *poGeom)
{
    if (iGeomField < 0 || iGeomField >= GetLayerDefn()->GetGeomFieldCount())
    {
        if (poGeom != nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid geometry field index : %d", iGeomField);
            return;
        }
    }

    m_iGeomFieldFilter = iGeomField;
    if (InstallFilter(poGeom))
        ResetReading();

    if (iCurLayer >= 0 && iCurLayer < nSrcLayers)
    {
        SetSpatialFilterToSourceLayer(papoSrcLayers[iCurLayer]);
    }
}

/************************************************************************/
/*                        TranslateFromSrcLayer()                       */
/************************************************************************/

OGRFeature *OGRUnionLayer::TranslateFromSrcLayer(OGRFeature *poSrcFeature)
{
    CPLAssert(poSrcFeature->GetFieldCount() == 0 || panMap != nullptr);
    CPLAssert(iCurLayer >= 0 && iCurLayer < nSrcLayers);

    OGRFeature *poFeature = new OGRFeature(poFeatureDefn);
    poFeature->SetFrom(poSrcFeature, panMap, TRUE);

    if (!osSourceLayerFieldName.empty() &&
        !poFeatureDefn->GetFieldDefn(0)->IsIgnored())
    {
        poFeature->SetField(0, papoSrcLayers[iCurLayer]->GetName());
    }

    for (int i = 0; i < poFeatureDefn->GetGeomFieldCount(); i++)
    {
        if (poFeatureDefn->GetGeomFieldDefn(i)->IsIgnored())
            poFeature->SetGeomFieldDirectly(i, nullptr);
        else
        {
            OGRGeometry *poGeom = poFeature->GetGeomFieldRef(i);
            if (poGeom != nullptr)
            {
                poGeom->assignSpatialReference(
                    poFeatureDefn->GetGeomFieldDefn(i)->GetSpatialRef());
            }
        }
    }

    if (bPreserveSrcFID)
        poFeature->SetFID(poSrcFeature->GetFID());
    else
        poFeature->SetFID(nNextFID++);
    return poFeature;
}

/************************************************************************/
/*                          SetIgnoredFields()                          */
/************************************************************************/

OGRErr OGRUnionLayer::SetIgnoredFields(CSLConstList papszFields)
{
    OGRErr eErr = OGRLayer::SetIgnoredFields(papszFields);
    if (eErr != OGRERR_NONE)
        return eErr;

    m_aosIgnoredFields = papszFields;

    return eErr;
}

/************************************************************************/
/*                             SyncToDisk()                             */
/************************************************************************/

OGRErr OGRUnionLayer::SyncToDisk()
{
    for (int i = 0; i < nSrcLayers; i++)
    {
        if (pabModifiedLayers[i])
        {
            papoSrcLayers[i]->SyncToDisk();
            pabModifiedLayers[i] = FALSE;
        }
    }

    return OGRERR_NONE;
}

#endif /* #ifndef DOXYGEN_SKIP */
