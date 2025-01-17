/*==LICENSE==*

CyanWorlds.com Engine - MMOG client, server and tools
Copyright (C) 2011  Cyan Worlds, Inc.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

Additional permissions under GNU GPL version 3 section 7

If you modify this Program, or any covered work, by linking or
combining it with any of RAD Game Tools Bink SDK, Autodesk 3ds Max SDK,
NVIDIA PhysX SDK, Microsoft DirectX SDK, OpenSSL library, Independent
JPEG Group JPEG library, Microsoft Windows Media SDK, or Apple QuickTime SDK
(or a modified version of those libraries),
containing parts covered by the terms of the Bink SDK EULA, 3ds Max EULA,
PhysX SDK EULA, DirectX SDK EULA, OpenSSL and SSLeay licenses, IJG
JPEG Library README, Windows Media SDK EULA, or QuickTime SDK EULA, the
licensors of this Program grant you additional
permission to convey the resulting work. Corresponding Source for a
non-source form of such a combination shall include the source code for
the parts of OpenSSL and IJG JPEG Library used as well as that of the covered
work.

You can contact Cyan Worlds, Inc. by email legal@cyan.com
 or by snail mail at:
      Cyan Worlds, Inc.
      14617 N Newport Hwy
      Mead, WA   99021

*==LICENSE==*/
//////////////////////////////////////////////////////////////////////////////
//                                                                          //
//  pfConsole Functions                                                     //
//                                                                          //
//////////////////////////////////////////////////////////////////////////////

#include "pfConsole.h"
#include "pfConsoleCore/pfConsoleEngine.h"

#include "HeadSpin.h"
#include "plFileSystem.h"
#include "plgDispatch.h"
#include "plPipeline.h"
#include "plProduct.h"
#include "hsTimer.h"

#include "plGImage/plPNG.h"
#include "plInputCore/plInputDevice.h"
#include "plInputCore/plInputInterface.h"
#include "plInputCore/plInputInterfaceMgr.h"
#include "pnInputCore/plKeyDef.h"
#include "pnInputCore/plKeyMap.h"
#include "pnKeyedObject/plFixedKey.h"
#include "plMessage/plInputEventMsg.h"
#include "plMessage/plCaptureRenderMsg.h"
#include "plMessage/plConsoleMsg.h"
#include "plMessage/plInputIfaceMgrMsg.h"
#include "plNetClient/plNetClientMgr.h"
#include "plPipeline/plDebugText.h"
#include "pfPython/cyPythonInterface.h"


//// Static Class Stuff //////////////////////////////////////////////////////

pfConsole   *pfConsole::fTheConsole = nil;
uint32_t      pfConsole::fConsoleTextColor = 0xff00ff00;
plPipeline  *pfConsole::fPipeline = nil;


//////////////////////////////////////////////////////////////////////////////
//// pfConsoleInputInterface - Input interface layer for the console /////////
//////////////////////////////////////////////////////////////////////////////

class pfConsoleInputInterface : public plInputInterface
{
    protected:

        pfConsole   *fConsole;



        virtual bool    IHandleCtrlCmd( plCtrlCmd *cmd )
        {
            if( cmd->fControlCode == B_SET_CONSOLE_MODE )
            {
                if( cmd->fControlActivated )
                {
                    // Activate/deactivate
                    switch( fConsole->fMode )
                    {
                        case pfConsole::kModeHidden:
                            fConsole->ISetMode( pfConsole::kModeSingleLine );
                            break;
                        case pfConsole::kModeSingleLine:
                            fConsole->ISetMode( pfConsole::kModeFull );
                            break;
                        case pfConsole::kModeFull:
                            fConsole->ISetMode( pfConsole::kModeHidden );
                            break;
                    }
                }
                return true;
            }
            return false;
        }

    public:

        pfConsoleInputInterface( pfConsole *console )
        {
            fConsole = console; 
            SetEnabled( true );         // Always enabled

            // Add our control codes to our control map. Do NOT add the key bindings yet.
            // Note: HERE is where you specify the actions for each command, i.e. net propagate and so forth.
            // This part basically declares us master of the bindings for these commands.
            
            // IF YOU ARE LOOKING TO CHANGE THE DEFAULT KEY BINDINGS, DO NOT LOOK HERE. GO TO
            // RestoreDefaultKeyMappings()!!!!

#ifndef PLASMA_EXTERNAL_RELEASE
            fControlMap->AddCode( B_SET_CONSOLE_MODE, kControlFlagNormal | kControlFlagNoRepeat );
#endif

            // IF YOU ARE LOOKING TO CHANGE THE DEFAULT KEY BINDINGS, DO NOT LOOK HERE. GO TO
            // RestoreDefaultKeyMappings()!!!!
        }

        virtual uint32_t  GetPriorityLevel() const          { return kConsolePriority; }
        virtual uint32_t  GetCurrentCursorID() const        { return kCursorHidden; }
        virtual bool    HasInterestingCursorID() const    { return false; }

        virtual bool    InterpretInputEvent( plInputEventMsg *pMsg )
        {
            plKeyEventMsg   *keyMsg = plKeyEventMsg::ConvertNoRef( pMsg );
            if( keyMsg != nil )
            {
                if( fConsole->fMode )
                {
                    fConsole->IHandleKey( keyMsg );
                    return true;
                }
            }

            return false;
        }

        virtual void    RefreshKeyMap()
        {
        }

        virtual void    RestoreDefaultKeyMappings()
        {
            if( fControlMap != nil )
            {
                fControlMap->UnmapAllBindings();
#ifndef PLASMA_EXTERNAL_RELEASE
                fControlMap->BindKey( KEY_TILDE, B_SET_CONSOLE_MODE );
#endif
            }
        }
};

//// Constructor & Destructor ////////////////////////////////////////////////

pfConsole::pfConsole()
    : fNumDisplayLines(32), fDisplayBuffer(), fFXEnabled(true), fEffectCounter(), fLastTime(),
      fHelpTimer(), fMode(), fInited(), fHelpMode(), fCursorTicks(), fMsgTimeoutTimer(),
      fPythonMode(), fPythonFirstTime(true), fPythonMultiLines(), fHistory(), fWorkingCursor(),
      fInputInterface(), fEngine()
{
    fTheConsole = this;
}

pfConsole::~pfConsole()
{
    if( fInputInterface != nil )
    {
        plInputIfaceMgrMsg *msg = new plInputIfaceMgrMsg( plInputIfaceMgrMsg::kRemoveInterface );
        msg->SetIFace( fInputInterface );
        plgDispatch::MsgSend( msg );

        hsRefCnt_SafeUnRef( fInputInterface );
        fInputInterface = nil;
    }

    if( fDisplayBuffer != nil )
    {
        delete [] fDisplayBuffer;
        fDisplayBuffer = nil;
    }

    fTheConsole = nil;

    plgDispatch::Dispatch()->UnRegisterForExactType( plConsoleMsg::Index(), GetKey() );
    plgDispatch::Dispatch()->UnRegisterForExactType( plControlEventMsg::Index(), GetKey() );
}

pfConsole * pfConsole::GetInstance () {

    return fTheConsole;
}

//// Init ////////////////////////////////////////////////////////////////////

void    pfConsole::Init( pfConsoleEngine *engine )
{
    fDisplayBuffer = new char[ fNumDisplayLines * kMaxCharsWide ];
    memset( fDisplayBuffer, 0, fNumDisplayLines * kMaxCharsWide );

    memset( fWorkingLine, 0, sizeof( fWorkingLine ) );
    fWorkingCursor = 0;

    memset( fHistory, 0, sizeof( fHistory ) );

    fEffectCounter = 0;
    fMode = 0;
    fMsgTimeoutTimer = 0;
    fHelpMode = false;
    fPythonMode = false;
    fPythonFirstTime = true;
    fPythonMultiLines = 0;
    fHelpTimer = 0;
    fCursorTicks = 0;
    memset( fLastHelpMsg, 0, sizeof( fLastHelpMsg ) );
    fEngine = engine;

    fInputInterface = new pfConsoleInputInterface( this );
    plInputIfaceMgrMsg *msg = new plInputIfaceMgrMsg( plInputIfaceMgrMsg::kAddInterface );
    msg->SetIFace( fInputInterface );
    plgDispatch::MsgSend( msg );

    // Register for keyboard event messages
    plgDispatch::Dispatch()->RegisterForExactType( plConsoleMsg::Index(), GetKey() );
    plgDispatch::Dispatch()->RegisterForExactType( plControlEventMsg::Index(), GetKey() );
}

//// ISetMode ////////////////////////////////////////////////////////////////

void    pfConsole::ISetMode( uint8_t mode )
{
    fMode = mode; 
    fEffectCounter = ( fFXEnabled ? kEffectDivisions : 0 ); 
    fMsgTimeoutTimer = 0; 
    fInputInterface->RefreshKeyMap();
}

//// MsgReceive //////////////////////////////////////////////////////////////

#include <algorithm>
bool    pfConsole::MsgReceive( plMessage *msg )
{
    // Handle screenshot saving...
    plCaptureRenderMsg* capMsg = plCaptureRenderMsg::ConvertNoRef(msg);
    if (capMsg) {
        plFileName screenshots = plFileName::Join(plFileSystem::GetUserDataPath(), "Screenshots");
        plFileSystem::CreateDir(screenshots, false); // just in case...
        ST::string prefix = plProduct::ShortName();

        // List all of the PNG indices we have taken up already...
        ST::string pattern = ST::format("{}*.png", prefix);
        std::vector<plFileName> images = plFileSystem::ListDir(screenshots, pattern.c_str());
        std::set<uint32_t> indices;
        std::for_each(images.begin(), images.end(),
            [&] (const plFileName& fn) {
                ST::string idx = fn.GetFileNameNoExt().substr(prefix.size());
                indices.insert(idx.to_uint(10));
            }
        );

        // Now that we have an ordered set of indices, save this screenshot to the first one we don't have.
        uint32_t num = 0;
        for (auto it = indices.begin(); it != indices.end(); ++it, ++num) {
            if (*it != num)
                break;
        }

        // Got our num, save the screenshot.
        plFileName fn = ST::format("{}{04}.png", prefix, num);
        plPNG::Instance().WriteToFile(plFileName::Join(screenshots, fn), capMsg->GetMipmap());

        AddLineF("Saved screenshot as '%s'", fn.AsString().c_str());
        return true;
    }

    plControlEventMsg *ctrlMsg = plControlEventMsg::ConvertNoRef( msg );
    if( ctrlMsg != nil )
    {
        if( ctrlMsg->ControlActivated() && ctrlMsg->GetControlCode() == B_CONTROL_CONSOLE_COMMAND && plNetClientMgr::GetInstance()->GetFlagsBit(plNetClientMgr::kPlayingGame))
        {
            fEngine->RunCommand( ctrlMsg->GetCmdString(), IAddLineCallback );
            return true;
        }
        return false;
    }

    plConsoleMsg *cmd = plConsoleMsg::ConvertNoRef( msg );
    if( cmd != nil && cmd->GetString() != nil )
    {
        if( cmd->GetCmd() == plConsoleMsg::kExecuteFile )
        {
            if( !fEngine->ExecuteFile( cmd->GetString() ) )
            {
                // Change the following line once we have a better way of reporting
                // errors in the parsing
                ST::string str = ST::format("Error parsing {}", cmd->GetString());
                ST::string msg = ST::format("{}:\n\nCommand: '{}'\n{}", fEngine->GetErrorMsg(), fEngine->GetLastErrorLine(),
#ifdef HS_DEBUGGING
                        "" );

                hsAssert( false, msg.c_str() );
#else
                        "\nPress OK to continue parsing files." );

                hsMessageBox( msg.c_str(), str.c_str(), hsMessageBoxNormal );
#endif
            }
        }
        else if( cmd->GetCmd() == plConsoleMsg::kAddLine )
            IAddParagraph( cmd->GetString() );
        else if( cmd->GetCmd() == plConsoleMsg::kExecuteLine )
        {
            if( !fEngine->RunCommand( (char *)cmd->GetString(), IAddLineCallback ) )
            {
                // Change the following line once we have a better way of reporting
                // errors in the parsing
                AddLineF("%s:\n\nCommand: '%s'\n", fEngine->GetErrorMsg(), fEngine->GetLastErrorLine());
            }
        }
            
        return true;
    }

    return hsKeyedObject::MsgReceive(msg);
}

//// IHandleKey //////////////////////////////////////////////////////////////

void    pfConsole::IHandleKey( plKeyEventMsg *msg )
{
    char            *c;
    wchar_t         key;
    int             i,eol;
    static bool     findAgain = false;
    static uint32_t   findCounter = 0;

    // filter out keyUps and ascii control characters
    // as the control functions are handled on the keyDown event
    if( !msg->GetKeyDown() || (msg->GetKeyChar() > '\0' && msg->GetKeyChar() < ' '))
        return;

    if( msg->GetKeyCode() == KEY_ESCAPE )
    {
        fWorkingLine[ fWorkingCursor = 0 ] = 0;
        findAgain = false;
        findCounter = 0;
        fHelpMode = false;
        fPythonMode = false;
        fPythonMultiLines = 0;
        IUpdateTooltip();
    }
    else if( msg->GetKeyCode() == KEY_TAB )
    {
        if ( fPythonMode )
        {
            // if we are in Python mode, then just add two spaces, tab over, sorta
            if ( strlen(fWorkingLine) < kWorkingLineSize+2 )
            {
                strcat(&fWorkingLine[fWorkingCursor], "  ");
                fWorkingCursor += 2;
            }
        }
        else
        {
            static char     lastSearch[ kMaxCharsWide ];
            char            search[ kMaxCharsWide ];

            if( !findAgain && findCounter == 0 )
                strcpy( lastSearch, fWorkingLine );
            strcpy( search, lastSearch );

            if( findCounter > 0 )
            {
                // Not found the normal way; try using an unrestricted search
                if( fEngine->FindNestedPartialCmd( search, findCounter, true ) )
                {
                    strcpy( fWorkingLine, search );
                    findCounter++;
                }
                else
                {
                    /// Try starting over...?
                    findCounter = 0;
                    if( fEngine->FindNestedPartialCmd( search, findCounter, true ) )
                    { 
                        strcpy( fWorkingLine, search );
                        findCounter++;
                    }
                }
            }
            else if( fEngine->FindPartialCmd( search, findAgain, true ) )
            {
                strcpy( fWorkingLine, search );
                findAgain = true;
            }
            else if( findAgain )
            {
                /// Try starting over
                strcpy( search, lastSearch );
                if( fEngine->FindPartialCmd( search, findAgain = false, true ) )
                {
                    strcpy( fWorkingLine, search );
                    findAgain = true;
                }
            }

            else
            {
                // Not found the normal way; start an unrestricted search
                if( fEngine->FindNestedPartialCmd( search, findCounter, true ) )
                {
                    strcpy( fWorkingLine, search );
                    findCounter++;
                }
            }

            fWorkingCursor = strlen( fWorkingLine );
            IUpdateTooltip();
        }
    }
    else if( msg->GetKeyCode() == KEY_UP )
    {
        i = ( fHistory[ fPythonMode ].fRecallCursor > 0 ) ? fHistory[ fPythonMode ].fRecallCursor - 1 : kNumHistoryItems - 1;
        if( fHistory[ fPythonMode ].fData[ i ][ 0 ] != 0 )
        {
            fHistory[ fPythonMode ].fRecallCursor = i;
            strcpy( fWorkingLine, fHistory[ fPythonMode ].fData[ fHistory[ fPythonMode ].fRecallCursor ] );
            findAgain = false;
            findCounter = 0;
            fWorkingCursor = strlen( fWorkingLine );
            IUpdateTooltip();
        }
    }
    else if( msg->GetKeyCode() == KEY_DOWN )
    {
        if( fHistory[ fPythonMode ].fRecallCursor != fHistory[ fPythonMode ].fCursor )
        {
            i = ( fHistory[ fPythonMode ].fRecallCursor < kNumHistoryItems - 1 ) ? fHistory[ fPythonMode ].fRecallCursor + 1 : 0;
            if( i != fHistory[ fPythonMode ].fCursor )
            {
                fHistory[ fPythonMode ].fRecallCursor = i;
                strcpy( fWorkingLine, fHistory[ fPythonMode ].fData[ fHistory[ fPythonMode ].fRecallCursor ] );
            }
            else
            {
                memset( fWorkingLine, 0, sizeof( fWorkingLine ) );
                fHistory[ fPythonMode ].fRecallCursor = fHistory[ fPythonMode ].fCursor;
            }
            findAgain = false;
            findCounter = 0;
            fWorkingCursor = strlen( fWorkingLine );
            IUpdateTooltip();
        }
    }
    else if( msg->GetKeyCode() == KEY_LEFT )
    {
        if( fWorkingCursor > 0 )
            fWorkingCursor--;
    }
    else if( msg->GetKeyCode() == KEY_RIGHT )
    {
        if( fWorkingCursor < strlen( fWorkingLine ) )
            fWorkingCursor++;
    }
    else if( msg->GetKeyCode() == KEY_BACKSPACE )
    {
        if( fWorkingCursor > 0 )
        {
            fWorkingCursor--;

            c = &fWorkingLine[ fWorkingCursor ];
            do
            {
                *c = *( c + 1 );
                c++;
            } while( *c != 0 );

            findAgain = false;
            findCounter = 0;
        }
        else if( fHelpMode )
            fHelpMode = false;

        IUpdateTooltip();
    }
    else if( msg->GetKeyCode() == KEY_DELETE )
    {
        c = &fWorkingLine[ fWorkingCursor ];
        do
        {
            *c = *( c + 1 );
            c++;
        } while( *c != 0 );

        findAgain = false;
        findCounter = 0;
        IUpdateTooltip();
    }
    else if( msg->GetKeyCode() == KEY_ENTER )
    {
        // leave leading space for Python multi lines (need the indents!)
        if ( fPythonMultiLines == 0 )
        {
            // Clean up working line by removing any leading whitespace
            for( c = fWorkingLine; *c == ' ' || *c == '\t'; c++ );
            for( i = 0; *c != 0; i++, c++ )
                fWorkingLine[ i ] = *c;
            fWorkingLine[ i ] = 0;
            eol = i;
        }

        if( fWorkingLine[ 0 ] == 0 && !fHelpMode && !fPythonMode )
        {
            // Blank line--just print a blank line to the console and skip
            IAddLine( "" );
            return;
        }

        // only save history line if there is something there
        if( fWorkingLine[ 0 ] != 0 )
        {
            // Save to history
            strcpy( fHistory[ fPythonMode ].fData[ fHistory[ fPythonMode ].fCursor ], fWorkingLine );
            fHistory[ fPythonMode ].fCursor = ( fHistory[ fPythonMode ].fCursor < kNumHistoryItems - 1 ) ? fHistory[ fPythonMode ].fCursor + 1 : 0;
            fHistory[ fPythonMode ].fRecallCursor = fHistory[ fPythonMode ].fCursor;
        }

        // EXECUTE!!! (warning: DESTROYS fWorkingLine)
        if( fHelpMode )
        {
            if( fWorkingLine[ 0 ] == 0 )
                IPrintSomeHelp();
            else if( stricmp( fWorkingLine, "commands" ) == 0 )
            {
                char empty[] = "";
                fEngine->PrintCmdHelp( empty, IAddLineCallback );
            }
            else if( !fEngine->PrintCmdHelp( fWorkingLine, IAddLineCallback ) )
            {
                c = (char *)fEngine->GetErrorMsg();
                AddLine( c );
            }

            fHelpMode = false;
        }
        // are we in Python mode?
        else if ( fPythonMode )
        {
            // are we in Python multi-line mode?
            if ( fPythonMultiLines > 0 )
            {
                // if there was a line then bump num lines
                if ( fWorkingLine[0] != 0 )
                {
                    AddLineF("... %s", fWorkingLine);
                    fPythonMultiLines++;
                }

                // is it time to evaluate all the multi lines that are saved?
                if ( fWorkingLine[0] == 0 || fPythonMultiLines >= kNumHistoryItems )
                {
                    if ( fPythonMultiLines >= kNumHistoryItems )
                        AddLine("Python Multi-line buffer full!");
                    // get the lines and stuff them in our buffer
                    char biglines[kNumHistoryItems*(kMaxCharsWide+1)];
                    biglines[0] = 0;
                    for ( i=fPythonMultiLines; i>0 ; i--)
                    {
                        // reach back in the history and find this line and paste it in here
                        int recall = fHistory[ fPythonMode ].fCursor - i;
                        if ( recall < 0 )
                            recall += kNumHistoryItems;
                        strcat(biglines,fHistory[ fPythonMode ].fData[ recall ]);
                        strcat(biglines,"\n");
                    }
                    // now evaluate this mess they made
                    PyObject* mymod = PythonInterface::FindModule("__main__");
                    PythonInterface::RunStringInteractive(biglines,mymod);
                    std::string output;
                    // get the messages
                    PythonInterface::getOutputAndReset(&output);
                    AddLine( output.c_str() );
                    // all done doing multi lines...
                    fPythonMultiLines = 0;
                }
            }
            // else we are just doing single lines
            else
            {
                // was there actually anything in the input buffer?
                if ( fWorkingLine[0] != 0 )
                {
                    AddLineF(">>> %s", fWorkingLine);
                    // check to see if this is going to be a multi line mode ( a ':' at the end)
                    if ( fWorkingLine[eol-1] == ':' )
                    {
                        fPythonMultiLines = 1;
                    }
                    else
                    // else if not the start of a multi-line then execute it
                    {
                        PyObject* mymod = PythonInterface::FindModule("__main__");
                        PythonInterface::RunStringInteractive(fWorkingLine,mymod);
                        std::string output;
                        // get the messages
                        PythonInterface::getOutputAndReset(&output);
                        AddLine( output.c_str() );
                    }
                }
                else
                    AddLine( ">>> " );
            }
            // find the end of the line
            for( c = fWorkingLine, eol = 0; *c != 0; eol++, c++ );
        }
        else
        {
            if( !fEngine->RunCommand( fWorkingLine, IAddLineCallback ) )
            {
                c = (char *)fEngine->GetErrorMsg();
                if( c[ 0 ] != 0 )
                    AddLine( c );
            }
        }

        // Clear
        fWorkingLine[ fWorkingCursor = 0 ] = 0;
        findAgain = false;
        findCounter = 0;
        IUpdateTooltip();
    }
    else if( msg->GetKeyCode() == KEY_END )
    {
        fWorkingCursor = strlen( fWorkingLine );
    }
    else if( msg->GetKeyCode() == KEY_HOME )
    {
        fWorkingCursor = 0;
    }
    else if (msg->GetKeyChar())
    {
        key = msg->GetKeyChar();
        // do they want to go into help mode?
        if( !fPythonMode && key == L'?' && fWorkingCursor == 0 )
        {
            /// Go into help mode
            fHelpMode = true;
        }
        // do they want to go into Python mode?
        else if( !fHelpMode && key == L'\\' && fWorkingCursor == 0 )
        {
            // toggle Python mode
            fPythonMode = fPythonMode ? false:true;
            if ( fPythonMode )
            {
                if ( fPythonFirstTime )
                {
                    IAddLine( "" );     // add a blank line
                    PythonInterface::RunStringInteractive("import sys;print(f'Python {sys.version}')", nullptr);
                    std::string output;
                    // get the messages
                    PythonInterface::getOutputAndReset(&output);
                    AddLine( output.c_str() );
                    fPythonFirstTime = false;       // do this only once!
                }
            }
        }
        // or are they just typing in a working line
        else if( strlen( fWorkingLine ) < kMaxCharsWide - 2 && key != 0 )
        {
            for( i = strlen( fWorkingLine ) + 1; i > fWorkingCursor; i-- )
                fWorkingLine[ i ] = fWorkingLine[ i - 1 ];

            // TODO: What about keys outside of `char` range?
            fWorkingLine[fWorkingCursor++] = char(key);

            findAgain = false;
            findCounter = 0;
            IUpdateTooltip();
        }
    }
}

//// IAddLineCallback ////////////////////////////////////////////////////////

void    pfConsole::IAddLineCallback( const char *string )
{
    fTheConsole->IAddParagraph( string, 0 );
}

//// IAddLine ////////////////////////////////////////////////////////////////

void    pfConsole::IAddLine( const char *string, short leftMargin )
{
    int         i;
    char        *ptr;


    /// Advance upward
    for( i = 0, ptr = fDisplayBuffer; i < fNumDisplayLines - 1; i++ )
    {
        memcpy( ptr, ptr + kMaxCharsWide, kMaxCharsWide );
        ptr += kMaxCharsWide;
    }

    if( string[ 0 ] == '\t' )
    {
        leftMargin += 4;
        string++;
    }

    memset( ptr, 0, kMaxCharsWide );
    memset( ptr, ' ', leftMargin );
    strncpy( ptr + leftMargin, string, kMaxCharsWide - leftMargin - 1 );
    if( fMode == 0 )
    {
        /// Console is invisible, so show this line for a bit of time
        fMsgTimeoutTimer = kMsgHintTimeout;
    }
}

//// IAddParagraph ///////////////////////////////////////////////////////////

void    pfConsole::IAddParagraph( const char *s, short margin )
{
    char        *ptr, *ptr2, *ptr3, *string=(char*)s;


    // Special character: if \i is in front of the string, indent it
    while( strncmp( string, "\\i", 2 ) == 0 )
    {
        margin += 3;
        string += 2;
    }

    for( ptr = string; ptr != nil && *ptr != 0; )
    {
        // Go as far as possible
        if( strlen( ptr ) < kMaxCharsWide - margin - margin - 1 )
            ptr2 = ptr + strlen( ptr );
        else
        {
            // Back up until we hit a sep
            ptr2 = ptr + kMaxCharsWide - margin - margin - 1;
            for( ; ptr2 > string && *ptr2 != ' ' && *ptr2 != '\t' && *ptr2 != '\n'; ptr2-- );
        }

        // Check for carriage return
        ptr3 = strchr( ptr, '\n' );
        if( ptr3 == ptr )
        {
            IAddLine( "", margin );
            ptr++;
            continue;
        }
        if( ptr3 != nil && ptr3 < ptr2 )
            ptr2 = ptr3;

        // Add this part
        if( ptr2 == ptr || *ptr2 == 0 )
        {
            IAddLine( ptr, margin );
            break;
        }

        *ptr2 = 0;
        IAddLine( ptr, margin );
        ptr = ptr2 + 1;
    }
}

//// IClear //////////////////////////////////////////////////////////////////

void    pfConsole::IClear()
{
    memset( fDisplayBuffer, 0, kMaxCharsWide * fNumDisplayLines );
}

//// Draw ////////////////////////////////////////////////////////////////////

void    pfConsole::Draw( plPipeline *p )
{
    int         i, yOff, y, x, eOffset, height;
    char        *line;
    char        tmp[ kMaxCharsWide ];
    bool        showTooltip = false;
    float       thisTime;   // For making the console FX speed konstant regardless of framerate
    const float kEffectDuration = 0.5f;


    plDebugText&    drawText = plDebugText::Instance();

    thisTime = hsTimer::GetSeconds<float>();

    if( fMode == kModeHidden && fEffectCounter == 0 )
    {
        if( fMsgTimeoutTimer > 0 )
        {
            /// Message hint--draw the last line of the console for a bit
            drawText.DrawString( 10, 4, fDisplayBuffer + kMaxCharsWide * ( fNumDisplayLines - 1 ), fConsoleTextColor );
            fMsgTimeoutTimer--;
        }
        fLastTime = thisTime;
        return;
    }

    drawText.SetDrawOnTopMode( true );

    yOff = drawText.GetFontHeight() + 2;
    if( fMode == kModeSingleLine )
        height = yOff * 3 + 14;
    else
        height = yOff * ( fNumDisplayLines + 2 ) + 14;

    if( fHelpTimer == 0 && !fHelpMode && fLastHelpMsg[ 0 ] != 0 )
        showTooltip = true;
    
    if( fEffectCounter > 0 )
    {
        int numElapsed = (int)( (float)kEffectDivisions * ( ( thisTime - fLastTime ) / (float)kEffectDuration ) );
        if( numElapsed > fEffectCounter )
            numElapsed = fEffectCounter;
        else if( numElapsed < 0 )
            numElapsed = 0;

        if( fMode == kModeSingleLine )
            eOffset = fEffectCounter * height / kEffectDivisions;
        else if( fMode == kModeFull )
            eOffset = fEffectCounter * ( height - yOff * 3 - 14 ) / kEffectDivisions;
        else
            eOffset = ( kEffectDivisions - fEffectCounter ) * ( height - yOff * 3 - 14 ) / kEffectDivisions;
        fEffectCounter -= numElapsed;
    }
    else 
        eOffset = 0;
    fLastTime = thisTime;

    if( fMode == kModeSingleLine )
    {
        // Bgnd (TEMP ONLY)
        x = kMaxCharsWide * drawText.CalcStringWidth( "W" ) + 4;
        y = height - eOffset;
        drawText.DrawRect( 4, 0, x, y, /*color*/0, 0, 0, 127 );

        /// Actual text
        if( fEffectCounter == 0 )
            drawText.DrawString( 10, 4, "Plasma 2.0 Console", 255, 255, 255, 255 );

        if( !showTooltip )
            drawText.DrawString( 10, 4 + yOff - eOffset, fDisplayBuffer + kMaxCharsWide * ( fNumDisplayLines - 1 ), fConsoleTextColor );

        y = 4 + yOff + yOff - eOffset;
    }
    else
    {
        // Bgnd (TEMP ONLY)
        x = kMaxCharsWide * drawText.CalcStringWidth( "W" ) + 4;
        y = yOff * ( fNumDisplayLines + 2 ) + 14 - eOffset;
        drawText.DrawRect( 4, 0, x, y, /*color*/0, 0, 0, 127 );

        /// Actual text
        drawText.DrawString( 10, 4, "Plasma 2.0 Console", 255, 255, 255, 255 );

        static int  countDown = 3000;
        if( fHelpTimer > 0 || fEffectCounter > 0 || fMode != kModeFull )
            countDown = 3000;
        else if( countDown > -720 )
            countDown--;

        // Resource data is encrypted so testers can't peer in to the EXE, plz don't decrypt
        static bool rezLoaded = false;
        static char tmpSrc[ kMaxCharsWide ];
        if( !rezLoaded )
        {
            memset( tmp, 0, sizeof( tmp ) );
            memset( tmpSrc, 0, sizeof( tmpSrc ) );
            // Our concession to windows
#ifdef HS_BUILD_FOR_WIN32
            #include "../../Apps/plClient/res/resource.h"
            HRSRC rsrc = FindResource( nil, MAKEINTRESOURCE( IDR_CNSL1 ), "CNSL" );
            if( rsrc != nil )
            {
                HGLOBAL hdl = LoadResource( nil, rsrc );
                if( hdl != nil )
                {
                    uint8_t *ptr = (uint8_t *)LockResource( hdl );
                    if( ptr != nil )
                    {
                        for( i = 0; i < SizeofResource( nil, rsrc ); i++ )
                            tmpSrc[ i ] = ptr[ i ] + 26;
                        UnlockResource( hdl );
                    }
                }
            }
            rezLoaded = true;
#else
            // Need to define for other platforms?
#endif
        }
        memcpy( tmp, tmpSrc, sizeof( tmp ) );

        if( countDown <= 0 )
        {
            y = 4 + yOff - eOffset;
            if( countDown <= -480 )
            {
                tmp[ ( (-countDown - 480)>> 4 ) + 1 ] = 0;
                drawText.DrawString( 10, y, tmp, fConsoleTextColor );
            }
            y += yOff * ( fNumDisplayLines - ( showTooltip ? 1 : 0 ) );
        }
        else
        {
            for( i = 0, y = 4 + yOff - eOffset, line = fDisplayBuffer; i < fNumDisplayLines - ( showTooltip ? 1 : 0 ); i++ )
            {
                drawText.DrawString( 10, y, line, fConsoleTextColor );
                y += yOff;
                line += kMaxCharsWide;
            }
        }

        if( showTooltip )
            y += yOff;
    }

//  strcpy( tmp, fHelpMode ? "Get Help On:" : "]" );
    if ( fHelpMode )
        strcpy( tmp, "Get Help On:");
    else if (fPythonMode )
        if ( fPythonMultiLines == 0 )
            strcpy( tmp, ">>>");
        else
            strcpy( tmp, "...");
    else
        strcpy( tmp, "]" );

    drawText.DrawString( 10, y, tmp, 255, 255, 255, 255 );
    i = 19 + drawText.CalcStringWidth( tmp );
    drawText.DrawString( i, y, fWorkingLine, fConsoleTextColor );

    if( fCursorTicks >= 0 )
    {
        strcpy( tmp, fWorkingLine );
        tmp[ fWorkingCursor ] = 0;
        x = drawText.CalcStringWidth( tmp );
        drawText.DrawString( i + x, y + 2, "_", 255, 255, 255 );
    }
    fCursorTicks--;
    if( fCursorTicks < -kCursorBlinkRate )
        fCursorTicks = kCursorBlinkRate;

    if( showTooltip )
        drawText.DrawString( i, y - yOff, fLastHelpMsg, 255, 255, 0 );
    else
        fHelpTimer--;

    drawText.SetDrawOnTopMode( false );
}

//// IUpdateTooltip //////////////////////////////////////////////////////////

void    pfConsole::IUpdateTooltip()
{
    char    tmpStr[ kWorkingLineSize ];
    char    *c;


    strcpy( tmpStr, fWorkingLine );
    c = (char *)fEngine->GetCmdSignature( tmpStr );
    if( c == nil || strcmp( c, fLastHelpMsg ) != 0 )
    {
        /// Different--update timer to wait
        fHelpTimer = kHelpDelay;
        strncpy( fLastHelpMsg, c ? c : "", kMaxCharsWide - 2 );
    }
}

//// IPrintSomeHelp //////////////////////////////////////////////////////////

void    pfConsole::IPrintSomeHelp()
{
    char    msg1[] = "The console contains commands arranged under groups and subgroups. \
To use a command, you type the group name plus the command, such as 'Console.Clear' or \
'Console Clear'.";
    
    char    msg2[] = "To get help on a command or group, type '?' followed by the command or \
group name. Typing '?' and just hitting enter will bring up this message. Typing '?' and \
then 'commands' will bring up a list of all base groups and commands.";

    char    msg3[] = "You can also have the console auto-complete a command by pressing tab. \
This will search for a group or command that starts with what you have typed. If there is more \
than one match, pressing tab repeatedly will cycle through all the matches.";


    AddLine( "" );
    AddLine( "How to use the console:" );
    IAddParagraph( msg1, 2 );
    AddLine( "" );
    IAddParagraph( msg2, 2 );
    AddLine( "" );
    IAddParagraph( msg3, 2 );
    AddLine( "" );
}


//============================================================================
void pfConsole::AddLineF(const char * fmt, ...) {
    char str[1024];
    va_list args;
    va_start(args, fmt);
    hsVsnprintf(str, std::size(str), fmt, args);
    va_end(args);
    AddLine(str);
}

//============================================================================
void pfConsole::RunCommandAsync (const char cmd[]) {

    plConsoleMsg * consoleMsg = new plConsoleMsg;
    consoleMsg->SetCmd(plConsoleMsg::kExecuteLine);
    consoleMsg->SetString(cmd);
//  consoleMsg->SetBreakBeforeDispatch(true);
    consoleMsg->Send(nil, true);
}
