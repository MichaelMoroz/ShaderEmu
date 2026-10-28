OpenMSX - base music set for OpenTTD
Readme

===============================
Current Version: OpenMSX 0.4.2
===============================

Contents
0 About OpenMSX
1 Installing
2 Troubleshooting
3 Contributing and compiling from source
4 License
5 Credits



0 About OpenMSX
===============

OpenMSX is an open source base music set for OpenTTD.



1 Installing
============

1. First, make sure that you downloaded and installed at least OpenTTD version
   1.0.0 or later.

2. Obtain OpenMSX:
    There are two ways, one easy (ingame download) and one complicated by
    downloading it yourself and installing it in the right place:
    a) Via ingame content download:
        - Go to the content download, and search for OpenMSX; it's found in
        the baseset category.

    b) Manually:
        download the latest OpenMSX package from its development homepage:
        homepage: https://github.com/OpenTTD/OpenMSX
        Unpack the zip file into the OpenTTD 'baseset' directory (see section 4.2
        of the OpenTTD readme for a detailed treatise on all data dirs OpenTTD
        recognizes).
        - An OpenTTD folder in your user account's home directory:
            Windows:
              C:\My Documents (95, 98, ME)
              C:\Documents and Settings\<username>\My Documents\OpenTTD (2000, XP)
              C:\Users\<username>\Documents\OpenTTD (Vista, 7)
          macOS:     ~/Documents/OpenTTD
          GNU/Linux: ~/.openttd
        - The OpenTTD installation directory.

3. In the main menu of the game, click the Game Options button. The Game
   Options dialog will appear.

4. Select OpenMSX from the drop-down list below Base Music set if that's not
   selected already (bottom left of window). Close the window using the × in the
   upper left corner.

   Now that wasn't so hard, was it? Anyways, if you're having trouble getting
   OpenMSX to work, please file a detailed report on what you did, what error
   messages you got and where you got stuck at our bug tracker at
       https://github.com/OpenTTD/OpenMSX/issues
   or at
       https://www.tt-forums.net

5. Music might not start automatically. If music does not play yet,
   start a game and open the music box from the toolbar.
   Here you can control the volume, and start and stop the music.


2 Troubleshooting and bad sound output
======================================

(Windows)
Depending upon the hardware and the music driver used, OpenTTD may have problems
to playback some songs without error or that it may happen that subsequent sound
output is changed in a detrimental way. The latter is especially know to happen
with the windows "dmusic" sound driver. You may want to try the "win32" music
driver, if you have problems with Windows' default dmusic driver. Search your
openttd.cfg for "musicdriver" and change the line to "musidriver = win32".
Consult also the OpenTTD readme for available options and its known-bugs.txt
for a more detailed and possibly more up to date description of Windows music
driver issues.

(GNU/Linux)
If you cannot get music to play, it might be because your system does not
support MIDI playback yet.
Please refer to the manual of your Linux distribution to learn about how
to get MIDI files to play.


3 Contributing and compiling from source
========================================

The OpenMSX source is available in a Git repository or as gzip'ed tarball.
You can do a clone from
   https://github.com/OpenTTD/OpenMSX
e.g. using
   git clone https://github.com/OpenTTD/OpenMSX
or obtain the tarball from
   https://www.openttd.org/downloads/openmsx-releases/latest.html

Prerequisites to building OpenMSX:
- Git (only when not building from a tarball)
- md5sum (Linux, MinGW) or md5 (macOS)
- some GNU utils: make, cat, sed
- unix2dos for convenient conversion of the text files
- Python
and you might additionally want a text editor of your choice and possibly a
programme capable of creating and editing midi files.

On Windows: We advise to get a MinGW development environment.
For more detailed instructions, see our guide
at http://dev.openttdcoop.org/projects/home/wiki and the very extensive and
detailed installation instructions on the MinGW wiki at
http://www.mingw.org/wiki/Getting_Started

On GNU/Linux: Your system probably has most of these things installed,
but if not, all of these things should be easy to install if your system
has a software package manager.

On macOS: Install the developers tools. Git is easiest installed via
macports: sudo port install git

The use of Git is strongly encouraged as only that allows to keep track of
changes.

Once all tools are installed, get a checkout of the repository and you can build
OpenMSX using 'make'. The following targets are available:
- all: builds all grfs and the obm file
- install: build and then copy OpenMSX in your OpenTTD music directory. Use
Makefile.local to specify a different path
- clean: cleans all generated files
- mrproper: also cleans generated directories
- bundle_src: create a source tarball
- bundle_zip: create a zip archive of OpenMSX
- bundle_bz2: create a bzip2 archive of OpenMSX
- bundle_tar: create a tar archive of OpenMSX
- check: checks the md5 sums of the built grf and obg files against those of
the official release versions

Given the usual case that you modify something within OpenMSX and want to test
that, a simple 'make install' should suffice and you can immediately test the
changes ingame, if you selected the nightly version of OpenMSX. Given default
paths, a 'make install' will overwrite a previous nightly version of OpenMSX.
Mind to re-start OpenTTD as it needs to re-read the files.



4 License
=========

The OpenMSX music set for OpenTTD Copyright (C) 2010-2021 OpenMSX Authors
(see below or in the source in themes.list) and is licensed under GPL v2.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License version 2 as published by the Free
Software Foundation.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 1 Franklin
Street, Fifth Floor, Boston, MA 02110-1301 USA.



5 Credits
=========

OpenMSX is created by the following people:

Programming and coordination:
    Ingo von Borstel (planetmaker)
Music critic and advisor:
    kamnet
Musicians:
  All 'Ezy Street' tracks
  The Hobo
    Jim Redfarn <http://homepage.ntlworld.com/jim.redfarn/MidiPage.htm>
  All 'Modern' tracks
    Tistou Blomberg
  Downtown cab ride
  Skydive
  Train filled with cash
  Unidentified Flying Scotsman
  Chugga Chugga Cha-ching
    imuh3
  Busy Schedule
  The Fast Route
    mimm
  Modern Motion
    Kyle Timmerman (Mr.Ksoft)
  Trolley Trev
    Keep on Rolling
  OpenTTD journey
    -lucas-


Song notes:
  Moo Moo Boogie: inspired by Cow Cow Boogie
                  by Benny Carter, Gene de Paul and Don Raye
  Careless Love:  is a traditional song in the Public Domain
