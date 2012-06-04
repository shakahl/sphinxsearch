//
// $Id$
//

//
// Copyright (c) 2001-2006, Andrew Aksyonoff. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "sphinx.h"
#include "sphinxutils.h"
#include <time.h>


#define CONF_CHECK(_hash,_key,_msg,_add) \
	if (!( _hash.Exists ( _key ) )) \
	{ \
		fprintf ( stdout, "ERROR: key '%s' not found " _msg, _key, _add ); \
		continue; \
	}


int main ( int argc, char ** argv )
{
	fprintf ( stdout, SPHINX_BANNER );
	if ( argc<=1 )
	{
		fprintf ( stdout,
			"Usage: search [OPTIONS] <word1 [word2 [word3 [...]]]>\n"
			"\n"
			"Options are:\n"
			"-c, --config <file>\tuse given config file (default: sphinx.conf)\n"
			"-i, --index <index>\tsearch given index only (default: all indexes)\n"
			"-a, --any\t\tmatch any query word (default: match all words)\n"
			"-b, --boolean\t\tmatch in boolean mode\n"
			"-f, --filter <attr> <v>\tonly match if attribute attr value is v\n"
			"-s, --start <offset>\tprint matches starting from this offset (default: 0)\n"
			"-l, --limit <count>\tprint this many matches (default: 20)\n"
			"-q, --noinfo\t\tdon't print document info from SQL database\n"
			"-g, --group <attr>\tgroup by attribute named attr\n"
			"--sort=date\t\tsort by date, descending\n"
			"--rsort=date\t\tsort by date, ascending\n"
			"--sort=ts\t\tsort by time segments\n"
			"--stdin\t\t\tread query from stdin\n"
		);
		exit ( 0 );
	}

	///////////////////////////////////////////
	// get query and other commandline options
	///////////////////////////////////////////

	CSphQuery tQuery;
	char sQuery [ 1024 ];
	sQuery[0] = '\0';

	const char * sConfName = "sphinx.conf";
	const char * sIndex = NULL;
	bool bNoInfo = false;
	bool bStdin = false;
	int iStart = 0;
	int iLimit = 20;

	#define OPT(_a1,_a2)	else if ( !strcmp(argv[i],_a1) || !strcmp(argv[i],_a2) )
	#define OPT1(_a1)		else if ( !strcmp(argv[i],_a1) )

	int i;
	for ( i=1; i<argc; i++ )
	{
		if ( argv[i][0]=='-' )
		{
			// this is an option
			if ( i==0 );
			OPT ( "-a", "--any" )		tQuery.m_eMode = SPH_MATCH_ANY;
			OPT ( "-b", "--boolean" )	tQuery.m_eMode = SPH_MATCH_BOOLEAN;
			OPT ( "-p", "--phrase" )	tQuery.m_eMode = SPH_MATCH_PHRASE;
			OPT ( "-e", "--ext" )		tQuery.m_eMode = SPH_MATCH_EXTENDED;
			OPT ( "-q", "--noinfo" )	bNoInfo = true;
			OPT1 ( "--sort=date" )		tQuery.m_eSort = SPH_SORT_ATTR_DESC;
			OPT1 ( "--rsort=date" )		tQuery.m_eSort = SPH_SORT_ATTR_ASC;
			OPT1 ( "--sort=ts" )		tQuery.m_eSort = SPH_SORT_TIME_SEGMENTS;
			OPT1 ( "--stdin" )			bStdin = true;

			else if ( (i+1)>=argc )		break;
			OPT ( "-s", "--start" )		iStart = atoi ( argv[++i] );
			OPT ( "-l", "--limit" )		iLimit = atoi ( argv[++i] );
			OPT ( "-c", "--config" )	sConfName = argv[++i];
			OPT ( "-i", "--index" )		sIndex = argv[++i];
			OPT ( "-g", "--group" )		{ tQuery.m_eGroupFunc = SPH_GROUPBY_ATTR; tQuery.m_sGroupBy = argv[++i]; }

			else if ( (i+2)>=argc )		break;
			OPT ( "-f", "--filter" )
			{
				if ( atoi ( argv[i+2] ) )
				{
					tQuery.m_dFilters.Reset ();
					tQuery.m_dFilters.Resize ( 1 );
					tQuery.m_dFilters[0].m_iValues = 1;
					tQuery.m_dFilters[0].m_pValues = new DWORD [ 1 ];
					tQuery.m_dFilters[0].m_pValues[0] = atoi ( argv[i+2] );
					tQuery.m_dFilters[0].m_sAttrName = argv[i+1];
					i += 2;
				}
			}

			else break; // unknown option

		} else if ( strlen(sQuery) + strlen(argv[i]) + 1 < sizeof(sQuery) )
		{
			// this is a search term
			strcat ( sQuery, argv[i] );
			strcat ( sQuery, " " );
		}
	}
	iStart = Max ( iStart, 0 );
	iLimit = Max ( iLimit, 0 );

	if ( i!=argc )
	{
		fprintf ( stdout, "ERROR: malformed or unknown option near '%s'.\n", argv[i] );
		return 1;
	}

	#undef OPT

	if ( bStdin )
	{
		int iPos = 0, iLeft = sizeof(sQuery)-1;
		char sThrowaway [ 256 ];

		while ( !feof(stdin) )
		{
			if ( iLeft>0 )
			{
				int iLen = fread ( sQuery, 1, iLeft, stdin );
				iPos += iLen;
				iLeft -= iLen;
			} else
			{
				fread ( sThrowaway, 1, sizeof(sThrowaway), stdin );
			}
		}

		assert ( iPos<(int)sizeof(sQuery) );
		sQuery[iPos] = '\0';
	}

	/////////////
	// configure
	/////////////

	tQuery.m_iMaxMatches = 1000; // iStart + iLimit;

	// load config
	CSphConfigParser cp;
	if ( !cp.Parse ( sConfName ) )
		sphDie ( "FATAL: failed to parse config file '%s'.\n", sConfName );

	CSphConfig & hConf = cp.m_tConf;
	if ( !hConf.Exists ( "index" ) )
		sphDie ( "FATAL: no indexes found in config file '%s'.\n", sConfName );

	/////////////////////
	// search each index
	/////////////////////

	hConf["index"].IterateStart ();
	while ( hConf["index"].IterateNext () )
	{
		const CSphConfigSection & hIndex = hConf["index"].IterateGet ();
		const char * sIndexName = hConf["index"].IterateGetKey().cstr();

		if ( sIndex && strcmp ( sIndex, sIndexName ) )
			continue;

		if ( hIndex("type") && hIndex["type"]=="distributed" )
			continue;

		if ( !hIndex.Exists ( "path" ) )
			sphDie ( "FATAL: key 'path' not found in index '%s'.\n", sIndexName );

		// configure charset_type
		tQuery.m_pTokenizer = NULL;
		bool bUseUTF8 = false;
		if ( hIndex.Exists ( "charset_type" ) )
		{
			if ( hIndex["charset_type"]=="sbcs" )
				tQuery.m_pTokenizer = sphCreateSBCSTokenizer ();
			else if ( hIndex["charset_type"]=="utf-8" )
			{
				tQuery.m_pTokenizer = sphCreateUTF8Tokenizer ();
				bUseUTF8 = true;
			}
			else
				sphDie ( "FATAL: unknown charset type '%s' in index '%s'.\n", hIndex["charset_type"].cstr(), sIndexName );
		} else
		{
			tQuery.m_pTokenizer = sphCreateSBCSTokenizer ();
		}

		// get morphology type
		DWORD iMorph = SPH_MORPH_NONE;
		if ( hIndex ( "morphology" ) )
		{
			iMorph = sphParseMorphology ( hIndex["morphology"], bUseUTF8 );
			if ( iMorph==SPH_MORPH_UNKNOWN )
				fprintf ( stdout, "WARNING: unknown morphology type '%s' ignored in index '%s'.\n",
					hIndex["morphology"].cstr(), sIndexName );
		}

		// do we want to show document info from database?
		#if USE_MYSQL
		MYSQL tSqlDriver;
		const char * sQueryInfo = NULL;

		while ( !bNoInfo )
		{
			if ( !hIndex.Exists ( "source" )
				|| !hConf.Exists ( "source" )
				|| !hConf["source"].Exists ( hIndex["source"] ) )
			{
				break;
			}
			const CSphConfigSection & hSource = hConf["source"][ hIndex["source"] ];

			if ( !hSource.Exists ( "sql_host" )
				|| !hSource.Exists ( "sql_user" )
				|| !hSource.Exists ( "sql_db" )
				|| !hSource.Exists ( "sql_pass" ) )
			{
				break;
			}

			if ( !hSource.Exists ( "sql_query_info" ) )
				break;
			sQueryInfo = hSource["sql_query_info"].cstr();
			if ( !strstr ( sQueryInfo, "$id" ) )
				sphDie ( "FATAL: 'sql_query_info' value must contain '$id'." );

			int iPort = 3306;
			if ( hSource.Exists ( "sql_port" ) && hSource["sql_port"].intval() )
				iPort = hSource["sql_port"].intval();

			mysql_init ( &tSqlDriver );
			if ( !mysql_real_connect ( &tSqlDriver,
				hSource["sql_host"].cstr(),
				hSource["sql_user"].cstr(),
				hSource["sql_pass"].cstr(),
				hSource["sql_db"].cstr(),
				iPort,
				hSource.Exists ( "sql_sock" ) ? hSource["sql_sock"].cstr() : NULL,
				0 ) )
			{
				sphDie ( "FATAL: failed to connect to MySQL (error='%s').",
					mysql_error ( &tSqlDriver ) );
			}

			// all good
			break;
		}
		#endif

		// configure stopwords
		CSphDict * pDict = new CSphDict_CRC32 ( iMorph );
		pDict->LoadStopwords ( hIndex.Exists ( "stopwords" ) ? hIndex["stopwords"].cstr() : NULL,
			tQuery.m_pTokenizer );

		// configure charset_table
		assert ( tQuery.m_pTokenizer );
		if ( hIndex.Exists ( "charset_table" ) )
			if ( !tQuery.m_pTokenizer->SetCaseFolding ( hIndex["charset_table"].cstr() ) )
				sphDie ( "FATAL: failed to parse 'charset_table' in index '%s'.\n", sIndexName );

		// min word len
		if ( hIndex("min_word_len") )
			tQuery.m_pTokenizer->SetMinWordLen ( hIndex["min_word_len"].intval() );

		//////////
		// search
		//////////

		tQuery.m_sQuery = sQuery;
		CSphQueryResult * pResult = NULL;

		CSphIndex * pIndex = sphCreateIndexPhrase ( hIndex["path"].cstr() );
		const CSphSchema * pSchema = pIndex->Preload ();
		if ( pSchema )
		{
			// setup groupby
			tQuery.SetSchema ( *pSchema );
			if ( !tQuery.m_sGroupBy.IsEmpty() && tQuery.GetGroupByAttr()<0 )
			{
				fprintf ( stdout, "WARNING: no such groupby attribute '%s' - DISABLING GROUPBY\n", tQuery.m_sGroupBy.cstr() );
				tQuery.m_sGroupBy = "";
			}

			// if we're not sorting by relevance, lookup first timestamp column
			if ( tQuery.m_eSort!=SPH_SORT_RELEVANCE )
			{
				int iTS = -1;
				ARRAY_FOREACH ( i, pSchema->m_dAttrs )
					if ( pSchema->m_dAttrs[i].m_eAttrType==SPH_ATTR_TIMESTAMP )
				{
					tQuery.m_sSortBy = pSchema->m_dAttrs[i].m_sName;
					iTS = i;
					break;
				}
				if ( iTS<0 )
				{
					fprintf ( stdout, "index '%s': no timestamp attributes found, sorting by relevance.\n", sIndexName );
					tQuery.m_eSort = SPH_SORT_RELEVANCE;
				}
			}

			pResult = pIndex->Query ( pDict, &tQuery );
		}

		SafeDelete ( pIndex );
		SafeDelete ( pDict );
		SafeDelete ( tQuery.m_pTokenizer );

		/////////
		// print
		/////////

		if ( !pResult )
		{
			fprintf ( stdout, "index '%s': search error: can not open index.\n", sIndexName );
			return 1;
		}

		fprintf ( stdout, "index '%s': query '%s': returned %d matches of %d total in %d.%03d sec\n",
			sIndexName, sQuery, pResult->m_dMatches.GetLength(), pResult->m_iTotalMatches,
			pResult->m_iQueryTime/1000, pResult->m_iQueryTime%1000 );

		if ( pResult->m_dMatches.GetLength() )
		{
			fprintf ( stdout, "\ndisplaying matches:\n" );

			int iMaxIndex = Min ( iStart+iLimit, pResult->m_dMatches.GetLength() );
			for ( int i=iStart; i<iMaxIndex; i++ )
			{
				CSphMatch & tMatch = pResult->m_dMatches[i];
				fprintf ( stdout, "%d. document=%d, weight=%d", 1+i, tMatch.m_iDocID, tMatch.m_iWeight );

				if ( tMatch.m_pAttrs )
					ARRAY_FOREACH ( j, pResult->m_tSchema.m_dAttrs )
				{
					const CSphColumnInfo & tAttr = pResult->m_tSchema.m_dAttrs[j];

					if ( tAttr.m_eAttrType==SPH_ATTR_INTEGER )
					{
						fprintf ( stdout, ", %s=%d", tAttr.m_sName.cstr(), tMatch.m_pAttrs[j] );
					
					} else if ( tAttr.m_eAttrType==SPH_ATTR_TIMESTAMP )
					{
						char sBuf[256];
						time_t tStamp = tMatch.m_pAttrs[j]; // for 64-bit
						strncpy ( sBuf, ctime(&tStamp), sizeof(sBuf) );

						char * p = sBuf;
						while ( (*p) && (*p)!='\n' && (*p)!='\r' ) p++;
						*p = '\0';

						fprintf ( stdout, ", %s=%s", tAttr.m_sName.cstr(), sBuf );
					}
				}
				fprintf ( stdout,"\n" );

				#if USE_MYSQL
				if ( sQueryInfo )
				{
					char * sQuery = sphStrMacro ( sQueryInfo, "$id", tMatch.m_iDocID );
					const char * sError = NULL;

					#define LOC_MYSQL_ERROR(_arg) { sError = _arg; break; }
					for ( ;; )
					{
						if ( mysql_query ( &tSqlDriver, sQuery ) )
							LOC_MYSQL_ERROR ( "mysql_query" );

						MYSQL_RES * pSqlResult = mysql_use_result ( &tSqlDriver );
						if ( !pSqlResult )
							LOC_MYSQL_ERROR ( "mysql_use_result" );

						MYSQL_ROW tRow = mysql_fetch_row ( pSqlResult );
						if ( !tRow )
							LOC_MYSQL_ERROR ( "mysql_fetch_row" );

						for ( int iField=0; iField<(int)pSqlResult->field_count; iField++ )
							fprintf ( stdout, "\t%s=%s\n", pSqlResult->fields[iField].name, tRow[iField] );

						mysql_free_result ( pSqlResult );
						break;
					}

					if ( sError )
						sphDie ( "FATAL: sql_query_info: %s: %s.\n", sError, mysql_error ( &tSqlDriver ) );

					delete [] sQuery;
				}
				#endif
			}

			fprintf ( stdout, "\nwords:\n" );
			for ( int i=0; i<pResult->m_iNumWords; i++ )
			{
				fprintf ( stdout, "%d. '%s': %d documents, %d hits\n",
					1+i,
					pResult->m_tWordStats[i].m_sWord.cstr(),
					pResult->m_tWordStats[i].m_iDocs,
					pResult->m_tWordStats[i].m_iHits );
			}
			fprintf ( stdout, "\n" );
		}
	}
}

//
// $Id$
//