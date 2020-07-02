#!/usr/bin/env python3

import sys
import os
from subprocess import run
import filecmp
import shutil
import pathlib

g_unitTests = \
[['Sample_PbsMaterials', 'Sample_PbsMaterials.json']]

print( 'Launched with ' + str( sys.argv ) )

if len( sys.argv ) != 5 and len( sys.argv ) != 6:
	print( 'Usage: ')
	print( '    python3 RunUnitTests.py metal|gl|d3d11 /pathTo/built/exes /pathTo/json_files ' \
			'/pathTo/binary_output /pathTo/old_cmp_binary_output' )
	print( 'Last argument can be skipped if generating the output (i.e. first run)' )
	exit( -1 )

g_api = sys.argv[1]
g_exeFolder = sys.argv[2]
g_jsonFolder = sys.argv[3]
g_outputFolder = sys.argv[4]
if len( sys.argv ) > 5:
	g_cmpFolder = sys.argv[5]
else:
	g_cmpFolder = ''

g_hasDifferentFiles = False

def compareResults( oldFolder, newFolder ):
	cmpResult = filecmp.dircmp( oldFolder, newFolder )
	if len( cmpResult.left_only ) > 0:
		g_hasDifferentFiles = True
		print( 'Warning: these files were not generated by this unit test but should have been:' )
		print( str( cmpResult.left_only ) )
	if len( cmpResult.right_only ) > 0:
		g_hasDifferentFiles = True
		print( 'Warning: these files were not in the original cmp folder:' )
		print( str( cmpResult.right_only ) )
	
	if len( cmpResult.diff_files ):
		print( 'All files equal' )
	else:
		g_hasDifferentFiles = True
		print( 'Different files: ' + str( cmpResult.diff_files ) )

def runUnitTest( exeName, jsonName ):
	exeFullpath = os.path.abspath( os.path.join( g_exeFolder, exeName ) )
	jsonFullpath = os.path.abspath( os.path.join( g_jsonFolder, jsonName ) )
	outputFolder = os.path.abspath( os.path.join( g_outputFolder, exeName ) )
	cmpFolder = os.path.abspath( os.path.join( g_cmpFolder, exeName ) )

	args = [exeFullpath, '--ut_playback=' + jsonFullpath, '--ut_output=' + outputFolder]
	print( 'Trying ' + str( args ) )
	print( 'Creating output folder ' + outputFolder )
	pathlib.Path( outputFolder ).mkdir( parents=True, exist_ok=True )
	processResult = run( args, cwd=g_exeFolder )
	processResult.check_returncode()

	if g_cmpFolder != '':
		compareResults( cmpFolder, outputFolder )

# Setup ogre.cfg
if g_api == 'gl':
	shutil.copyfile( './ogreGL.cfg', os.path.join( g_exeFolder, 'ogre.cfg' ) )

# Iterate through all tests and run it
for unitTest in g_unitTests:
	runUnitTest( unitTest[0], unitTest[1] )

if g_hasDifferentFiles:
	exit( -2 )
