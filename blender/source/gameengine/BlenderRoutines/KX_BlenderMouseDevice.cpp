/**
 * $Id$
 * ***** BEGIN GPL/BL DUAL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. The Blender
 * Foundation also sells licenses for use in proprietary software under
 * the Blender License.  See http://www.blender.org/BL/ for information
 * about this.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The Original Code is Copyright (C) 2001-2002 by NaN Holding BV.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL/BL DUAL LICENSE BLOCK *****
 */


#ifdef WIN32
// annoying warnings about truncated STL debug info
#pragma warning (disable :4786)
#endif 

#include "KX_BlenderMouseDevice.h"

KX_BlenderMouseDevice::KX_BlenderMouseDevice()
{

}
KX_BlenderMouseDevice::~KX_BlenderMouseDevice()
{

}

/**
	IsPressed gives boolean information about mouse status, true if pressed, false if not
*/

bool KX_BlenderMouseDevice::IsPressed(SCA_IInputDevice::KX_EnumInputs inputcode)
{
	const SCA_InputEvent & inevent =  m_eventStatusTables[m_currentTable][inputcode];
	bool pressed = (inevent.m_status == SCA_InputEvent::KX_JUSTACTIVATED || 
		inevent.m_status == SCA_InputEvent::KX_ACTIVE);
	return pressed;
}
/*const SCA_InputEvent&	KX_BlenderMouseDevice::GetEventValue(SCA_IInputDevice::KX_EnumInputs inputcode)
{
	return m_eventStatusTables[m_currentTable][inputcode];
}
*/

/** 
	NextFrame toggles currentTable with previousTable,
	and copy relevant event information from previous to current
	(pressed keys need to be remembered)
*/
void	KX_BlenderMouseDevice::NextFrame()
{
	SCA_IInputDevice::NextFrame();
	
	// now convert justpressed keyevents into regular (active) keyevents
	int previousTable = 1-m_currentTable;
	for (int mouseevent= KX_BEGINMOUSE; mouseevent< KX_ENDMOUSEBUTTONS;mouseevent++)
	{
		SCA_InputEvent& oldevent = m_eventStatusTables[previousTable][mouseevent];
		if (oldevent.m_status == SCA_InputEvent::KX_JUSTACTIVATED ||
			oldevent.m_status == SCA_InputEvent::KX_ACTIVE	)
		{
			m_eventStatusTables[m_currentTable][mouseevent] = oldevent;
			m_eventStatusTables[m_currentTable][mouseevent].m_status = SCA_InputEvent::KX_ACTIVE;
		}
	}
	for (int mousemove= KX_ENDMOUSEBUTTONS; mousemove< KX_ENDMOUSE;mousemove++)
	{
		SCA_InputEvent& oldevent = m_eventStatusTables[previousTable][mousemove];
		m_eventStatusTables[m_currentTable][mousemove] = oldevent;
		if (oldevent.m_status == SCA_InputEvent::KX_JUSTACTIVATED ||
			oldevent.m_status == SCA_InputEvent::KX_ACTIVE	)
		{
			
			m_eventStatusTables[m_currentTable][mousemove].m_status = SCA_InputEvent::KX_JUSTRELEASED;
		} else
		{
			if (oldevent.m_status == SCA_InputEvent::KX_JUSTRELEASED)
			{
				
				m_eventStatusTables[m_currentTable][mousemove].m_status = SCA_InputEvent::KX_NO_INPUTSTATUS ;
			}
		}
	}
}


/** 
	ConvertBlenderEvent translates blender mouse events into ketsji kbd events
	extra event information is stored, like ramp-mode (just released/pressed)
*/


bool	KX_BlenderMouseDevice::ConvertBlenderEvent(unsigned short incode,short val)
{
	bool result = false;
	
	// convert event
	KX_EnumInputs kxevent = this->ToNative(incode);

	// only process it, if it's a key
	if (kxevent > KX_BEGINMOUSE && kxevent < KX_ENDMOUSE)
	{
		int previousTable = 1-m_currentTable;


		if (val > 0)
		{
			m_eventStatusTables[m_currentTable][kxevent].m_eventval = val ; //???

			switch (m_eventStatusTables[previousTable][kxevent].m_status)
			{
			
			case SCA_InputEvent::KX_ACTIVE:
			case SCA_InputEvent::KX_JUSTACTIVATED:
				{
					m_eventStatusTables[m_currentTable][kxevent].m_status = SCA_InputEvent::KX_ACTIVE;
					break;
				}
			case SCA_InputEvent::KX_JUSTRELEASED:
				{
					if ( kxevent > KX_BEGINMOUSEBUTTONS && kxevent < KX_ENDMOUSEBUTTONS)
					{
						m_eventStatusTables[m_currentTable][kxevent].m_status = SCA_InputEvent::KX_JUSTACTIVATED;
					} else
					{
						m_eventStatusTables[m_currentTable][kxevent].m_status = SCA_InputEvent::KX_ACTIVE;
						
					}
					break;
				}
			default:
				{
					m_eventStatusTables[m_currentTable][kxevent].m_status = SCA_InputEvent::KX_JUSTACTIVATED;
				}
			}
			
		} else
		{
			// blender eventval == 0
			switch (m_eventStatusTables[previousTable][kxevent].m_status)
			{
			case SCA_InputEvent::KX_JUSTACTIVATED:
			case SCA_InputEvent::KX_ACTIVE:
				{
					m_eventStatusTables[m_currentTable][kxevent].m_status = SCA_InputEvent::KX_JUSTRELEASED;
					break;
				}
			default:
				{
					m_eventStatusTables[m_currentTable][kxevent].m_status = SCA_InputEvent::KX_NO_INPUTSTATUS;
				}
			}
		}
	}
	return result;
}
