/* MDM - The MDM Display Manager
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef GREETER_CONFIGURATION_H
#define GREETER_CONFIGURATION_H

extern gboolean MdmUseCirclesInEntry;
extern gboolean MdmUseInvisibleInEntry;
extern gboolean MdmShowGnomeFailsafeSession;
extern gboolean MdmShowXtermFailsafeSession;
extern gboolean MdmShowLastSession;
extern gboolean MdmSystemMenu;
extern gboolean MdmConfigAvailable;
extern gboolean MdmChooserButton;
extern gchar *MdmHalt;
extern gchar *MdmReboot;
extern gchar *MdmSuspend;
extern gchar *MdmConfigurator;
extern gboolean MdmHaltFound;
extern gboolean MdmRebootFound;
extern gboolean MdmCustomCmdFound;
extern gboolean *MdmCustomCmdsFound;
extern gboolean MdmAnyCustomCmdsFound;
extern gboolean MdmSuspendFound;
extern gboolean MdmConfiguratorFound;
extern gchar *MdmSessionDir;
extern gchar *MdmDefaultSession;
extern gchar *MdmDefaultLocale;
extern gchar *MdmLocaleFile;
extern gboolean MdmTimedLoginEnable;
extern gboolean MdmUse24Clock;
extern gchar *MdmTimedLogin;
extern gint MdmTimedLoginDelay;
extern gchar *MdmGlobalFaceDir;
extern gchar *MdmDefaultFace;
extern gint  MdmIconMaxHeight;
extern gint  MdmIconMaxWidth;
extern gchar *MdmExclude;
extern int MdmMinimalUID;
extern gboolean MdmAllowRoot;
extern gboolean MdmAllowRemoteRoot;
extern gchar *MdmWelcome;
extern gchar *MdmServAuthDir;
extern gchar *MdmInfoMsgFile;
extern gchar *MdmInfoMsgFont;
extern gchar *MdmSoundProgram;
extern gchar *MdmSoundOnLoginReadyFile;
extern gchar *MdmSoundOnLoginSuccessFile;
extern gchar *MdmSoundOnLoginFailureFile;
extern gboolean MdmSoundOnLoginReady;
extern gboolean MdmSoundOnLoginSuccess;
extern gboolean MdmSoundOnLoginFailure;

extern gboolean MDM_IS_LOCAL;
extern gboolean DOING_MDM_DEVELOPMENT;

#endif /* GREETER_CONFIGURATION_H */
