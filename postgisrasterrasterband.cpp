/******************************************************************************
 * File :    PostGISRasterRasterBand.cpp
 * Project:  PostGIS Raster driver
 * Purpose:  GDAL Raster Band implementation for PostGIS Raster driver 
 * Author:   Jorge Arevalo, jorgearevalo@deimos-space.com
 * 
 * Last changes:
 * $Id: $
 *
 ******************************************************************************
 * Copyright (c) 2009 - 2011, Jorge Arevalo, jorge.arevalo@deimos-space.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ******************************************************************************/
#include "postgisraster.h"
#include "ogr_api.h"
#include "ogr_geometry.h"
#include "gdal_priv.h"
#include "gdal.h"
#include <string>
#include "cpl_string.h"
#include "gdal_vrt.h"
#include "vrtdataset.h"
#include "memdataset.h"


/**
 * \brief Constructor.
 * Parameters:
 *  - PostGISRasterDataset *: The Dataset this band belongs to
 *  - int: the band number
 *  - GDALDataType: The data type for this band
 *  - double: The nodata value.  Could be any kind of data (GByte, GUInt16,
 *          GInt32...) but the variable has the bigger type.
 *  - GBool: if the data type is signed byte or not. If yes, the SIGNEDBYTE
 *          metadata value is added to IMAGE_STRUCTURE domain
 *  - int: The bit depth, to add NBITS as metadata value in IMAGE_STRUCTURE
 *          domain.
 *	TODO: Comment the rest of parameters
 */
PostGISRasterRasterBand::PostGISRasterRasterBand(PostGISRasterDataset *poDS,
        int nBand, GDALDataType hDataType, GBool bHasNoDataValue, double dfNodata, 
		GBool bSignedByte,int nBitDepth, int nFactor, int nBlockXSize, int nBlockYSize,
		GBool bIsOffline)
{

    /* Basic properties */
    this->poDS = poDS;
    this->nBand = nBand;
    this->bIsOffline = bIsOffline;

    eAccess = poDS->GetAccess();
    eDataType = hDataType;
    this->bHasNoDataValue = bHasNoDataValue;
    dfNoDataValue = dfNodata;


    /**************************************************************************
     * TODO: Set a non arbitrary blocksize. In case or regular blocking, this is 
	 * easy, otherwise, does it make any sense? Any single tile has its own 
	 * dimensions.
     *************************************************************************/
	if (poDS->bRegularBlocking) {
		CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::Constructor: "
			"Band %d has regular blocking", nBand);
	
		this->nBlockXSize = nBlockXSize;
		this->nBlockYSize = nBlockYSize;
	}

	else {
		CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::Constructor: "
			"Band %d does not have regular blocking", nBand);

		this->nBlockXSize = MIN(poDS->nRasterXSize, DEFAULT_BLOCK_X_SIZE); 
		this->nBlockYSize = MIN(poDS->nRasterYSize, DEFAULT_BLOCK_Y_SIZE);
	}

	CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::Constructor: "
		"Block size (%dx%d)", nBlockXSize, nBlockYSize);
        
    // Add pixeltype to image structure domain
    if (bSignedByte == true) {
        SetMetadataItem("PIXELTYPE", "SIGNEDBYTE", "IMAGE_STRUCTURE" );
    }

    // Add NBITS to metadata only for sub-byte types
    if (nBitDepth < 8)
        SetMetadataItem("NBITS", CPLString().Printf( "%d", nBitDepth ),
            "IMAGE_STRUCTURE" );


    nOverviewFactor = nFactor;

    /**********************************************************
     * Check overviews. Query RASTER_OVERVIEWS table for
     * existing overviews, only in case we are on level 0
     * TODO: can we do this without querying RASTER_OVERVIEWS?
     * How do we know the number of overviews? Is an inphinite
     * loop...
     **********************************************************/
    if (nOverviewFactor == 0) {    

        CPLString osCommand;
        PGresult * poResult = NULL;
        int i = 0;
        int nFetchOvFactor = 0;
        char * pszOvSchema = NULL;
        char * pszOvTable = NULL;
        char * pszOvColumn = NULL;

        nRasterXSize = poDS->GetRasterXSize();
        nRasterYSize = poDS->GetRasterYSize();
 
        osCommand.Printf("select o_table_name, overview_factor, o_raster_column, "
                "o_table_schema from raster_overviews where r_table_schema = "
                "'%s' and r_table_name = '%s' and r_raster_column = '%s'",
                poDS->pszSchema, poDS->pszTable, poDS->pszColumn);

        poResult = PQexec(poDS->poConn, osCommand.c_str());
        if (poResult != NULL && PQresultStatus(poResult) == PGRES_TUPLES_OK &&
                PQntuples(poResult) > 0) {
            
            /* Create overviews */
            nOverviewCount = PQntuples(poResult);           
            papoOverviews = (PostGISRasterRasterBand **)VSICalloc(nOverviewCount,
                    sizeof(PostGISRasterRasterBand *));
            if (papoOverviews == NULL) {
                CPLError(CE_Warning, CPLE_OutOfMemory, "Couldn't create "
                        "overviews for band %d\n", nBand);              
                PQclear(poResult);
                return;
            }
                       
            for(i = 0; i < nOverviewCount; i++) {
		
				CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::Constructor: "
					"Creating overview for band %d", nBand);

                nFetchOvFactor = atoi(PQgetvalue(poResult, i, 1));
                pszOvSchema = CPLStrdup(PQgetvalue(poResult, i, 3));
                pszOvTable = CPLStrdup(PQgetvalue(poResult, i, 0));
                pszOvColumn = CPLStrdup(PQgetvalue(poResult, i, 2));
 
                /**
                 * NOTE: Overview bands are not considered to be a part of a
                 * dataset, but we use the same dataset for all the overview
                 * bands just for simplification (we'll need to access the table
                 * and schema names). But in method GetDataset, NULL is return
                 * if we're talking about an overview band
                 */
                papoOverviews[i] = new PostGISRasterRasterBand(poDS, nBand,
                        hDataType, bHasNoDataValue, dfNodata, bSignedByte, nBitDepth,
                        nFetchOvFactor, nBlockXSize, nBlockYSize, bIsOffline);

            }

            PQclear(poResult);

        }

        else {
			
			CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::Constructor: "
				"Band %d does not have overviews", nBand);
            nOverviewCount = 0;
            papoOverviews = NULL;
            if (poResult)
                PQclear(poResult);
        }
    }

    /************************************
     * We are in an overview level. Set
     * raster size to its value 
     ************************************/
    else {

        /* 
         * No overviews inside an overview (all the overviews are from original
         * band
         */
        nOverviewCount = 0;
        papoOverviews = NULL;

        nRasterXSize = (int) floor((double)poDS->GetRasterXSize() / nOverviewFactor);
        nRasterYSize = (int) floor((double)poDS->GetRasterYSize() / nOverviewFactor);        
    }

    CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand constructor: Band "
            "created (srid = %d)", poDS->nSrid);
}

/***********************************************
 * \brief: Band destructor
 ***********************************************/
PostGISRasterRasterBand::~PostGISRasterRasterBand()
{
    int i;

    if (papoOverviews) {
        for(i = 0; i < nOverviewCount; i++)
            delete papoOverviews[i];

        CPLFree(papoOverviews);
    }
}
	

/**
 *
 **/
GDALDataType PostGISRasterRasterBand::TranslateDataType(const char * pszDataType)
{
	if (EQUALN(pszDataType, "1BB", 3 * sizeof (char)) ||
		EQUALN(pszDataType, "2BUI", 4 * sizeof (char)) ||
        EQUALN(pszDataType, "4BUI", 4 * sizeof (char)) ||
   		EQUALN(pszDataType, "8BUI", 4 * sizeof (char)) ||
        EQUALN(pszDataType, "8BSI", 4 * sizeof (char))) 
		
		return GDT_Byte;

	else if (EQUALN(pszDataType, "16BSI", 5 * sizeof (char)))
		return GDT_Int16;

	else if (EQUALN(pszDataType, "16BUI", 5 * sizeof (char)))
		return GDT_UInt16;
    
	else if (EQUALN(pszDataType, "32BSI", 5 * sizeof (char)))
		return GDT_Int32;
    
	else if (EQUALN(pszDataType, "32BUI", 5 * sizeof (char)))
		return GDT_UInt32;

	else if (EQUALN(pszDataType, "32BF", 4 * sizeof (char)))
		return GDT_Float32;

	else if (EQUALN(pszDataType, "64BF", 4 * sizeof (char)))
		return GDT_Float64;

	else
		return GDT_Unknown;
}



/**
 * Read/write a region of image data from multiple bands.
 *
 * This method allows reading a region of one or more PostGISRasterBands from
 * this dataset into a buffer. The write support is still under development
 *
 * The function fetches all the raster data that intersects with the region
 * provided, and store the data in the GDAL cache.
 *
 * TODO: This only works in case of regular blocking rasters. A more
 * general approach to allow non-regular blocking rasters is under development.
 *
 * It automatically takes care of data type translation if the data type
 * (eBufType) of the buffer is different than that of the
 * PostGISRasterRasterBand.
 *
 * TODO: The method should take care of image decimation / replication if the
 * buffer size (nBufXSize x nBufYSize) is different than the size of the region
 * being accessed (nXSize x nYSize).
 *
 * The nPixelSpace, nLineSpace and nBandSpace parameters allow reading into or
 * writing from various organization of buffers.
 *
 * @param eRWFlag Either GF_Read to read a region of data, or GF_Write to write
 * a region of data.
 *
 * @param nXOff The pixel offset to the top left corner of the region of the
 * band to be accessed. This would be zero to start from the left side.
 *
 * @param nYOff The line offset to the top left corner of the region of the band
 * to be accessed. This would be zero to start from the top.
 *
 * @param nXSize The width of the region of the band to be accessed in pixels.
 *
 * @param nYSize The height of the region of the band to be accessed in lines.
 *
 * @param pData The buffer into which the data should be read, or from which it
 * should be written. This buffer must contain at least
 * nBufXSize * nBufYSize * nBandCount words of type eBufType. It is organized in
 * left to right,top to bottom pixel order. Spacing is controlled by the
 * nPixelSpace, and nLineSpace parameters.
 *
 * @param nBufXSize the width of the buffer image into which the desired region
 * is to be read, or from which it is to be written.
 *
 * @param nBufYSize the height of the buffer image into which the desired region
 * is to be read, or from which it is to be written.
 *
 * @param eBufType the type of the pixel values in the pData data buffer. The
 * pixel values will automatically be translated to/from the
 * PostGISRasterRasterBand data type as needed.
 *
 * @param nBandCount the number of bands being read or written.
 *
 * @param panBandMap the list of nBandCount band numbers being read/written.
 * Note band numbers are 1 based. This may be NULL to select the first
 * nBandCount bands.
 *
 * @param nPixelSpace The byte offset from the start of one pixel value in pData
 * to the start of the next pixel value within a scanline. If defaulted (0) the
 * size of the datatype eBufType is used.
 *
 * @param nLineSpace The byte offset from the start of one scanline in pData to
 * the start of the next. If defaulted (0) the size of the datatype
 * eBufType * nBufXSize is used.
 *
 * @param nBandSpace the byte offset from the start of one bands data to the
 * start of the next. If defaulted (0) the value will be nLineSpace * nBufYSize
 * implying band sequential organization of the data buffer.
 *
 * @return CE_Failure if the access fails, otherwise CE_None.
 */

CPLErr PostGISRasterRasterBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
	int nXSize, int nYSize, void * pData, int nBufXSize, int nBufYSize,
	GDALDataType eBufType, int nPixelSpace, int nLineSpace)
{
    double adfTransform[6];
    double adfProjWin[8];
    int ulx, uly, lrx, lry;
    CPLString osCommand;
    PGresult* poResult = NULL;
	int iTuplesIndex;
    int nTuples = 0;
    char orderByY[5];
    char orderByX[4];
	GBool bEqualAreas = false;
    GByte* pbyData = NULL;
	GByte* pbyBandData = NULL;
    int nWKBLength = 0;
	int nBandDataLength;
	int nBandDataSize;
	int nBufDataSize;
	int nTileWidth;
	int nTileHeight;
	double dfTileScaleX;
	double dfTileScaleY;
	double dfTileUpperLeftX;
	double dfTileUpperLeftY;
	char * pszDataType = NULL; 
	char * pszDataTypeName = NULL;
	GDALDataType eTileDataType;
	int nTileDataTypeSize;
	double dfTileBandNoDataValue;
	VRTDatasetH vrtDataset;
	GDALDataset ** memDatasets;
	GDALRasterBandH memRasterBand;
	GDALRasterBandH vrtRasterBand;
	char szMemOpenInfo[100];
	GDALOpenInfo * oOpenInfo;
	char ** papszOptions;
	char szTmp[64];
	char szTileWidth[64];
	char szTileHeight[64];
	CPLErr err;
    PostGISRasterDataset * poPostGISRasterDS = (PostGISRasterDataset*)poDS;
	int nSrcXOff, nSrcYOff, nDstXOff, nDstYOff;
	int nDstXSize, nDstYSize;

	/**
     * TODO: Write support not implemented yet
     **/
    if (eRWFlag == GF_Write) {
        CPLError(CE_Failure, CPLE_NotSupported,
			"Writing through PostGIS Raster band not supported yet");
        
		return CE_Failure;
    }
    
	nBandDataSize = GDALGetDataTypeSize(eDataType) / 8;
    nBufDataSize = GDALGetDataTypeSize( eBufType ) / 8;
            

	/**************************************************************************
	 * Do we have overviews that would be appropriate to satisfy this request?                                                   
	 *************************************************************************/
	if( (nBufXSize < nXSize || nBufYSize < nYSize) && GetOverviewCount() > 0 ) {
		if( OverviewRasterIO( eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, 
			nBufXSize, nBufYSize, eBufType, nPixelSpace, nLineSpace ) == CE_None )
                
		return CE_None;
	}

  	/**************************************************************************
	 * Get all the raster rows that are intersected by the window requested
	 *************************************************************************/		
	// We first construct a polygon to intersect with
	poPostGISRasterDS->GetGeoTransform(adfTransform);
	ulx = nXOff;
	uly = nYOff;
	lrx = nXOff + nXSize * nBandDataSize;
	lry = nYOff + nYSize * nBandDataSize;

	adfProjWin[0] = adfTransform[GEOTRSFRM_TOPLEFT_X] + 
					ulx * adfTransform[GEOTRSFRM_WE_RES] + 
					uly * adfTransform[GEOTRSFRM_ROTATION_PARAM1];
	adfProjWin[1] = adfTransform[GEOTRSFRM_TOPLEFT_Y] + 
					ulx * adfTransform[GEOTRSFRM_ROTATION_PARAM2] + 
					uly * adfTransform[GEOTRSFRM_NS_RES];
	adfProjWin[2] = adfTransform[GEOTRSFRM_TOPLEFT_X] + 
					lrx * adfTransform[GEOTRSFRM_WE_RES] + 
					uly * adfTransform[GEOTRSFRM_ROTATION_PARAM1];
	adfProjWin[3] = adfTransform[GEOTRSFRM_TOPLEFT_Y] + 
					lrx * adfTransform[GEOTRSFRM_ROTATION_PARAM2] + 
					uly * adfTransform[GEOTRSFRM_NS_RES];
	adfProjWin[4] = adfTransform[GEOTRSFRM_TOPLEFT_X] + 
					lrx * adfTransform[GEOTRSFRM_WE_RES] + 
					lry * adfTransform[GEOTRSFRM_ROTATION_PARAM1];
	adfProjWin[5] = adfTransform[GEOTRSFRM_TOPLEFT_Y] + 
					lrx * adfTransform[GEOTRSFRM_ROTATION_PARAM2] + 
					lry * adfTransform[GEOTRSFRM_NS_RES];
	adfProjWin[6] = adfTransform[GEOTRSFRM_TOPLEFT_X] + 
					ulx * adfTransform[GEOTRSFRM_WE_RES] + 
					lry * adfTransform[GEOTRSFRM_ROTATION_PARAM1];
	adfProjWin[7] = adfTransform[GEOTRSFRM_TOPLEFT_Y] + 
					ulx * adfTransform[GEOTRSFRM_ROTATION_PARAM2] + 
					lry * adfTransform[GEOTRSFRM_NS_RES];

	memset(orderByX, 0, 4);
	memset(orderByY, 0, 5);

	strcpy(orderByX, "asc");
	if (poPostGISRasterDS->nSrid == -1) 
		strcpy(orderByY, "asc"); // Y starts at 0 and grows
	else 
		strcpy(orderByY, "desc");// Y starts at max and decreases

	CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO: "
		"Buffer size = (%d, %d), Region size = (%d, %d)",
		nBufXSize, nBufYSize, nXSize, nYSize);

	if (poPostGISRasterDS->pszWhere == NULL) {
		osCommand.Printf("SELECT st_band(%s, %d), st_width(%s), st_height(%s), st_bandpixeltype(%s, %d), "
			"st_bandnodatavalue(%s, %d), st_scalex(%s), st_scaley(%s), st_upperleftx(%s), st_upperlefty(%s) "
			"FROM %s.%s WHERE st_intersects(%s, st_polygonfromtext('POLYGON((%.17f %.17f, %.17f %.17f, "
			"%.17f %.17f, %.17f %.17f, %.17f %.17f))', %d)) ORDER BY ST_UpperLeftY(%s) %s, "
			"ST_UpperLeftX(%s) %s", poPostGISRasterDS->pszColumn, nBand, poPostGISRasterDS->pszColumn, 
			poPostGISRasterDS->pszColumn, poPostGISRasterDS->pszColumn, nBand, poPostGISRasterDS->pszColumn, 
			nBand, poPostGISRasterDS->pszColumn, poPostGISRasterDS->pszColumn, poPostGISRasterDS->pszColumn,
			poPostGISRasterDS->pszColumn, poPostGISRasterDS->pszSchema, poPostGISRasterDS->pszTable, 
			poPostGISRasterDS->pszColumn, adfProjWin[0], adfProjWin[1], adfProjWin[2], adfProjWin[3], 
			adfProjWin[4], adfProjWin[5], adfProjWin[6], adfProjWin[7], adfProjWin[0], adfProjWin[1], 
			poPostGISRasterDS->nSrid, poPostGISRasterDS->pszColumn, orderByY, poPostGISRasterDS->pszColumn, 
			orderByX);
	}

	else {
		osCommand.Printf("SELECT st_band(%s, %d), st_width(%s), st_height(%s), st_bandpixeltype(%s, %d), "
			"st_bandnodatavalue(%s, %d), st_scalex(%s), st_scaley(%s), st_upperleftx(%s), st_upperlefty(%s) "
			"FROM %s.%s WHERE %s AND st_intersects(%s, st_polygonfromtext('POLYGON((%.17f %.17f, %.17f %.17f, "
			"%.17f %.17f, %.17f %.17f, %.17f %.17f))', %d)) ORDER BY ST_UpperLeftY(%s) %s, "
			"ST_UpperLeftX(%s) %s", poPostGISRasterDS->pszColumn, nBand, poPostGISRasterDS->pszColumn, 
			poPostGISRasterDS->pszColumn, poPostGISRasterDS->pszColumn, nBand, poPostGISRasterDS->pszColumn, 
			nBand, poPostGISRasterDS->pszColumn,poPostGISRasterDS->pszColumn,poPostGISRasterDS->pszColumn,
			poPostGISRasterDS->pszColumn, poPostGISRasterDS->pszSchema, poPostGISRasterDS->pszTable, 
			poPostGISRasterDS->pszWhere, poPostGISRasterDS->pszColumn, adfProjWin[0], adfProjWin[1], 
			adfProjWin[2], adfProjWin[3], adfProjWin[4], adfProjWin[5], adfProjWin[6], adfProjWin[7], 
			adfProjWin[0], adfProjWin[1], poPostGISRasterDS->nSrid, poPostGISRasterDS->pszColumn, orderByY, 
			poPostGISRasterDS->pszColumn, orderByX);
	}

	CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO(): Query = %s", osCommand.c_str());

	poResult = PQexec(poPostGISRasterDS->poConn, osCommand.c_str());
	if (poResult == NULL || PQresultStatus(poResult) != PGRES_TUPLES_OK || 
		PQntuples(poResult) < 0) {
		
		if (poResult)
			PQclear(poResult);
 
		CPLError(CE_Failure, CPLE_AppDefined, "Error retrieving raster data from database");

		CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO(): %s", 
			PQerrorMessage(poPostGISRasterDS->poConn));
        
		return CE_Failure;	
	}

	/**
	 * No data. Return the buffer filled with nodata values
	 **/
	else if (PQntuples(poResult) == 0) {
		PQclear(poResult);
		
		CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO(): Null block");

		return CE_None;	
	}
	

	nTuples = PQntuples(poResult);

	/**************************************************************************
	 * Allocate memory for MEM dataset
	 * TODO: In case of memory error, provide a different alternative
	 *************************************************************************/
	memDatasets = (GDALDataset **)CPLCalloc(nTuples, sizeof(GDALDataset *));
	if (!memDatasets) {
		PQclear(poResult);	
		CPLError(CE_Failure, CPLE_AppDefined, "Memory error while trying to read band data "
			"from database");

		return CE_Failure;
	}
	

	/**************************************************************************
	 * Create an empty in-memory VRT dataset
	 * TODO: In case of memory error, provide a different alternative
	 *************************************************************************/
	vrtDataset = VRTCreate(nXSize, nYSize);
	if (!vrtDataset) {
		PQclear(poResult);
		CPLFree(memDatasets);

		CPLError(CE_Failure, CPLE_AppDefined, "Memory error while trying to read band data "
			"from database");

		return CE_Failure;
	}
	
	GDALSetDescription(vrtDataset, "postgis_raster.vrt");
	GDALSetProjection(vrtDataset, GDALGetProjectionRef((GDALDatasetH)this->poDS));
	GDALSetGeoTransform(vrtDataset, adfTransform);


	/**
	 * Create one VRT Raster Band. It will contain the same band of all tiles
	 * as Simple Sources
	 **/
	VRTAddBand(vrtDataset, eDataType, NULL);
	vrtRasterBand = GDALGetRasterBand(vrtDataset, 1);
	
	
	/**************************************************************************
	 * Now, for each block, create a MEM dataset
	 * TODO: What if whe have a really BIG amount of data fetched from db? CURSORS
	 *************************************************************************/
	for(iTuplesIndex = 0; iTuplesIndex < nTuples; iTuplesIndex++) {
	
		/**
		 * Fetch data from result
		 **/
		pbyData = CPLHexToBinary(PQgetvalue(poResult, iTuplesIndex, 0), &nWKBLength);
		nTileWidth = atoi(PQgetvalue(poResult, iTuplesIndex, 1));
		nTileHeight = atoi(PQgetvalue(poResult, iTuplesIndex, 2));
		pszDataType = CPLStrdup(PQgetvalue(poResult, iTuplesIndex, 3));
		dfTileBandNoDataValue = atof(PQgetvalue(poResult, iTuplesIndex, 4));
		dfTileScaleX = atof(PQgetvalue(poResult, iTuplesIndex, 5));
		dfTileScaleY = atof(PQgetvalue(poResult, iTuplesIndex, 6));
		dfTileUpperLeftX = atof(PQgetvalue(poResult, iTuplesIndex, 7));
		dfTileUpperLeftY = atof(PQgetvalue(poResult, iTuplesIndex, 8));
			
		/**
		 * Calculate some useful parameters
		 **/
		eTileDataType = TranslateDataType(pszDataType);
		nTileDataTypeSize = GDALGetDataTypeSize(eTileDataType) / 8;
		
		nBandDataLength = nTileWidth * nTileHeight * nTileDataTypeSize;

		/**
		 * Get the pointer to the band pixels
		 **/ 
		pbyBandData = GET_BAND_DATA(pbyData, 1, nTileDataTypeSize, nBandDataLength);
		
		/**
		 * Create new MEM dataset, based on in-memory array, to hold the pixels.
		 * The dataset will have only 1 band
		 **/
		memset(szTmp, 0, sizeof(szTmp));
		CPLPrintPointer(szTmp, pbyBandData, sizeof(szTmp));

		memset(szTileWidth, 0, sizeof(szTileWidth));
		CPLPrintInt32(szTileWidth, (GInt32)nTileWidth, sizeof(nTileWidth));
		memset(szTileHeight, 0, sizeof(szTileHeight));
		CPLPrintInt32(szTileHeight, (GInt32)nTileHeight, sizeof(nTileHeight));
		
		memset(szMemOpenInfo, 0, sizeof(szMemOpenInfo));
		sprintf(szMemOpenInfo, "MEM:::DATAPOINTER=%s,PIXELS=%d,LINES=%d,DATATYPE=%s",
			szTmp, nTileWidth, nTileHeight, GDALGetDataTypeName(eTileDataType));
		
		CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO: MEMDataset "
			"open info = %s", szMemOpenInfo);

		oOpenInfo = new GDALOpenInfo(szMemOpenInfo, GA_ReadOnly, NULL);
		if (!oOpenInfo) {
			CPLError(CE_Warning, CPLE_AppDefined, "Could not allocate memory for "
				"MEMDataset, skipping. The result image may contain gaps");
			continue;
		}
		
		memDatasets[iTuplesIndex] = MEMDataset::Open(oOpenInfo);
		if (!memDatasets[iTuplesIndex]) {
			delete oOpenInfo;
			CPLError(CE_Warning, CPLE_AppDefined, "Could not create MEMDataset, "
				"skipping. The result image may contain gaps");
			continue;
		}
		
		GDALSetDescription(memDatasets[iTuplesIndex], szMemOpenInfo);

		/** 
		 * Get MEM raster band, to add it as simple source.
		 **/
		memRasterBand = (GDALRasterBandH)memDatasets[iTuplesIndex]->GetRasterBand(1);
		if (!memRasterBand) {
			CPLError(CE_Warning, CPLE_AppDefined, "Could not get MEMRasterBand , "
				"skipping. The result image may contain gaps");
			continue;
		} 
		((MEMRasterBand *)memRasterBand)->SetNoDataValue(dfTileBandNoDataValue);

		CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO: Adding "
			"VRT Simple Source");
		
		/**
		 * Get source and destination windows for the simple source
		 **/ 
		if (dfTileUpperLeftX < poPostGISRasterDS->xmin) {
			nSrcXOff = (int)((poPostGISRasterDS->xmin - dfTileUpperLeftX) / 
				dfTileScaleX + 0.5);
        	nDstXOff = 0;
		}

		else {
			nSrcXOff = 0;
			nDstXOff = (int)(0.5 + (dfTileUpperLeftX - poPostGISRasterDS->xmin) / 	
				adfTransform[1]);
    	}

		if (poPostGISRasterDS->ymax < dfTileUpperLeftY) {
        	nSrcYOff = (int)((dfTileUpperLeftY - poPostGISRasterDS->ymax) / 
				fabs(dfTileScaleY) + 0.5);
			nDstYOff = 0;
    	}

		else {
			nSrcYOff = 0;
			nDstYOff = (int)(0.5 + (poPostGISRasterDS->ymax - dfTileUpperLeftY) / 
				fabs(adfTransform[5]));
		}

		nDstXSize = (int)(0.5 + nTileWidth * dfTileScaleX / adfTransform[1]);
		nDstYSize = (int)(0.5 + nTileHeight * fabs(dfTileScaleY) / fabs(adfTransform[5]));
     

		/**
		 * Add the mem raster band as new simple source band
		 **/
		VRTAddSimpleSource(vrtRasterBand, memRasterBand, nSrcXOff, nSrcYOff, nTileWidth, nTileHeight,
			nDstXOff, nDstYOff, nDstXSize, nDstYSize, NULL, dfTileBandNoDataValue);

		delete oOpenInfo;

		CPLFree(pbyData);
		CPLFree(pszDataType);

	}
 
	PQclear(poResult);
	
	// Just for testing (writes VRT file, with name = dataset description, to disk)
	VRTFlushCache(vrtDataset);

	CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO(): VRT file created");

	// Execute VRT RasterIO over the band
	err = ((VRTRasterBand *)vrtRasterBand)->RasterIO(eRWFlag, nXOff, nYOff, nXSize, 
		nYSize, pData, nBufXSize, nBufYSize, eBufType, nPixelSpace, nLineSpace);

	CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO(): Data read");

	GDALClose(vrtDataset);
	
	CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO(): VRTDataset released");
	
	// Free resources
	for(iTuplesIndex = 0; iTuplesIndex < nTuples; iTuplesIndex++) {
		delete memDatasets[iTuplesIndex];
		//GDALClose(memDatasets[iTuplesIndex]);
	}
	CPLFree(memDatasets);
	
	CPLDebug("PostGIS_Raster", "PostGISRasterRasterBand::IRasterIO(): MEMDatasets released");

	return err;
		
}

/**
 * \brief Set the no data value for this band.
 * Parameters:
 *  - double: The nodata value
 * Returns:
 *  - CE_None.
 */
CPLErr PostGISRasterRasterBand::SetNoDataValue(double dfNewValue) {
    dfNoDataValue = dfNewValue;

    return CE_None;
}

/**
 * \brief Fetch the no data value for this band.
 * Parameters:
 *  - int *: pointer to a boolean to use to indicate if a value is actually
 *          associated with this layer. May be NULL (default).
 *  Returns:
 *  - double: the nodata value for this band.
 */
double PostGISRasterRasterBand::GetNoDataValue(int *pbSuccess) {
    if (pbSuccess != NULL)
        *pbSuccess = (int) bHasNoDataValue;

    return dfNoDataValue;
}


/**
 * \brief Get the natural block size for this band.
 * Parameters:
 *  - int *: pointer to int to store the natural X block size
 *  - int *: pointer to int to store the natural Y block size
 * Returns: nothing
 */
void PostGISRasterRasterBand::GetBlockSize(int * pnXSize, int *pnYSize)
 {
    if (nBlockXSize == 0 || nBlockYSize == 0) {
        CPLError(CE_Failure, CPLE_AppDefined,
                "This PostGIS Raster band has non regular blocking arrangement. \
                This feature is under development");

        if (pnXSize != NULL)
            *pnXSize = 0;
        if (pnYSize != NULL)
            *pnYSize = 0;

    }
    else {
        GDALRasterBand::GetBlockSize(pnXSize, pnYSize);
    }
}


/*****************************************************
 * \brief Fetch the band number
 *****************************************************/
int PostGISRasterRasterBand::GetBand()
{
    return (nOverviewFactor) ? 0 : nBand;
}

/*****************************************************
 * \brief Fetch the owning dataset handle
 *****************************************************/
GDALDataset* PostGISRasterRasterBand::GetDataset()
{
    return (nOverviewFactor) ? NULL : poDS;
}

/****************************************************
 * \brief Check for arbitrary overviews
 * The datastore can compute arbitrary overviews 
 * efficiently, because the overviews are tables, 
 * like the original raster. The effort is the same.
 ****************************************************/
int PostGISRasterRasterBand::HasArbitraryOverviews()
{
    return (nOverviewFactor) ? false : true;
}

/***************************************************
 * \brief Return the number of overview layers available
 ***************************************************/
int PostGISRasterRasterBand::GetOverviewCount()
{
    return (nOverviewFactor) ?
        0 :
        nOverviewCount;
}

/**********************************************************
 * \brief Fetch overview raster band object
 **********************************************************/
GDALRasterBand * PostGISRasterRasterBand::GetOverview(int i)
{
    return (i >= 0 && i < GetOverviewCount()) ? 
        (GDALRasterBand *)papoOverviews[i] : GDALRasterBand::GetOverview(i);
}

/*****************************************************
 * \brief Read a natural block of raster band data
 *****************************************************/
CPLErr PostGISRasterRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void*
        pImage)
{
    int nPixelSize = GDALGetDataTypeSize(eDataType)/8;
    int nReadXSize, nReadYSize;

    if( (nBlockXOff+1) * nBlockXSize > GetXSize() )
        nReadXSize = GetXSize() - nBlockXOff * nBlockXSize;
    else
        nReadXSize = nBlockXSize;

    if( (nBlockYOff+1) * nBlockYSize > GetYSize() )
        nReadYSize = GetYSize() - nBlockYOff * nBlockYSize;
    else
        nReadYSize = nBlockYSize;

    return IRasterIO( GF_Read, 
                      nBlockXOff * nBlockXSize, nBlockYOff * nBlockYSize, 
                      nReadXSize, nReadYSize, 
                      pImage, nReadXSize, nReadYSize, eDataType, 
                      nPixelSize, nPixelSize * nBlockXSize );
}


