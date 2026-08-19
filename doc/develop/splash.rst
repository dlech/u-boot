.. SPDX-License-Identifier: GPL-2.0-or-later

Splash screen
=============

Enabling the splash screen
--------------------------

CONFIG_SPLASH_SCREEN checks the environment for a variable *splashimage*. If
found, the usual display of logo, copyright and system information on the LCD
is suppressed and the BMP image at the hexadecimal address given by
*splashimage* is loaded and shown instead. The console is redirected to the
"nulldev" too, allowing for a "silent" boot where a splash screen is shown very
quickly after power-on.

If *splashimage* is not set, no splash screen is displayed.

Normally the U-Boot version string is shown on the display once the splash
screen is enabled, since video starts up after U-Boot has displayed the initial
banner and the banner is otherwise not visible. CONFIG_HIDE_LOGO_VERSION hides
this version information.

Note that this version banner is only printed on the video console when the
splash image is left at its default position (0,0, see *splashpos* below). If
the image is moved away from 0,0, the banner is skipped regardless of
CONFIG_HIDE_LOGO_VERSION.

Preparing the splash image
--------------------------

The splash_screen_prepare() function is a weak function defined in
``common/splash.c``. It is called as part of the splash screen display sequence.
It gives the board an opportunity to prepare the splash image data before it is
processed and sent to the frame buffer by U-Boot. Define your own version to use
this feature.

If CONFIG_SPLASH_SOURCE is not enabled, the default splash_screen_prepare()
falls back to copying the compiled-in U-Boot logo (see CONFIG_VIDEO_LOGO) to the
address given by *splashimage*, so it is shown as the splash image instead of a
board-supplied one.

Selecting the splash image source
---------------------------------

CONFIG_SPLASH_SOURCE enables the splash_source.c library. This library provides
facilities to declare board specific splash image locations, routines for
loading a splash image from supported locations, and a way of controlling the
selected splash location using the *splashsource* environment variable.

*splashsource* works as follows:

* If *splashsource* is set to a supported location name as defined by board
  code, use that splash location.
* If *splashsource* is undefined, use the first splash location as default.
* If *splashsource* is set to an unsupported value, do not load a splash screen.

A splash source location can describe either storage with raw data, a storage
formatted with a file system or a FIT image. In case of a filesystem, the splash
screen data is loaded as a file. The name of the splash screen file can be
controlled with the environment variable *splashfile*.

To enable loading the splash image from a FIT image, CONFIG_FIT must be enabled.
The FIT image has to start at the 'offset' field address in the selected splash
location. The name of the splash image within the FIT shall be specified by the
environment variable *splashfile*.

In case the environment variable *splashfile* is not defined the default name
``splash.bmp`` will be used.

For storage backed locations (MMC, SATA, USB, etc.) the device and partition to
read from can be overridden with the environment variable *splashdevpart*. Its
value follows the same ``<dev>[:<part>]`` or ``<dev>#<partition name>``
conventions used by the load commands, e.g. ``0:1`` or ``0#splash``. When
*splashdevpart* is not set, the ``devpart`` field of the board's splash location
entry is used instead.

For raw storage and FIT locations, the offset to read from can be overridden
with the environment variable *splashoffset*, given in hexadecimal. When
*splashoffset* is not set, the ``offset`` field of the board's splash location
entry is used instead.

Positioning the splash image
----------------------------

If CONFIG_SPLASH_SCREEN_ALIGN is enabled, the splash image can be freely
positioned on the display using the environment variable *splashpos*, given as
``x,y``:

* A positive number is the number of pixels from the left/top.
* A negative number is the number of pixels from the right/bottom.
* ``m`` centers the image on that axis.

Examples::

  setenv splashpos m,m
  	=> image at center of screen

  setenv splashpos 30,20
  	=> image at x = 30 and y = 20

  setenv splashpos -10,m
  	=> vertically centered image
  	   at x = dspWidth - bmpWidth - 9

Splash screen in SPL
--------------------

The splash screen feature is also available in SPL, controlled by the SPL
counterparts of the above options: CONFIG_SPL_SPLASH_SCREEN,
CONFIG_SPL_SPLASH_SCREEN_ALIGN and CONFIG_SPL_SPLASH_SOURCE. They behave the
same as their non-SPL equivalents, but apply only at the SPL stage, and are
selected and configured independently of the U-Boot proper options.
