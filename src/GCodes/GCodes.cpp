/****************************************************************************************************

 RepRapFirmware - G Codes

 This class interprets G Codes from one or more sources, and calls the functions in Move, Heat etc
 that drive the machine to do what the G Codes command.

 Most of the functions in here are designed not to wait, and they return a boolean.  When you want them to do
 something, you call them.  If they return false, the machine can't do what you want yet.  So you go away
 and do something else.  Then you try again.  If they return true, the thing you wanted done has been done.

 -----------------------------------------------------------------------------------------------------

 Version 0.1

 13 February 2013

 Adrian Bowyer
 RepRap Professional Ltd
 http://reprappro.com

 Licence: GPL

 ****************************************************************************************************/

#include "RepRapFirmware.h"

#ifdef DUET_NG
#include "FirmwareUpdater.h"
#endif

#define DEGREE_SYMBOL	"\xC2\xB0"				// degree-symbol encoding in UTF8

const char GCodes::axisLetters[AXES] = { 'X', 'Y', 'Z' };

const size_t gcodeReplyLength = 2048;			// long enough to pass back a reasonable number of files in response to M20

GCodes::GCodes(Platform* p, Webserver* w) :
	platform(p), webserver(w), active(false), stackPointer(0), auxGCodeReply(nullptr), isFlashing(false),
	fileBeingHashed(nullptr)
{
	httpGCode = new GCodeBuffer(platform, "http", HTTP_MESSAGE);
	telnetGCode = new GCodeBuffer(platform, "telnet", TELNET_MESSAGE);
	fileGCode = new GCodeBuffer(platform, "file", GENERIC_MESSAGE);
	serialGCode = new GCodeBuffer(platform, "serial", HOST_MESSAGE);
	auxGCode = new GCodeBuffer(platform, "aux", AUX_MESSAGE);
	fileMacroGCode = new GCodeBuffer(platform, "macro", GENERIC_MESSAGE);
}

void GCodes::Exit()
{
	platform->Message(HOST_MESSAGE, "GCodes class exited.\n");
	active = false;
}

void GCodes::Init()
{
	Reset();
	distanceScale = 1.0;
	rawExtruderTotal = 0.0;
	for (size_t extruder = 0; extruder < DRIVES - AXES; extruder++)
	{
		lastRawExtruderPosition[extruder] = 0.0;
		rawExtruderTotalByDrive[extruder] = 0.0;
	}
	eofString = EOF_STRING;
	eofStringCounter = 0;
	eofStringLength = strlen(eofString);
	offSetSet = false;
	zProbesSet = false;
	active = true;
	longWait = platform->Time();
	dwellTime = longWait;
	limitAxes = true;
	for(size_t axis = 0; axis < AXES; axis++)
	{
		axisScaleFactors[axis] = 1.0;
	}
	SetAllAxesNotHomed();
	for (size_t i = 0; i < NUM_FANS; ++i)
	{
		pausedFanValues[i] = 0.0;
	}

	retractLength = retractExtra = retractSpeed = retractHop = 0.0;
}

// This is called from Init and when doing an emergency stop
void GCodes::Reset()
{
	httpGCode->Init();
	telnetGCode->Init();
	fileGCode->Init();
	serialGCode->Init();
	auxGCode->Init();
	auxGCode->SetCommsProperties(1);					// by default, we require a checksum on the aux port
	fileMacroGCode->Init();
	moveAvailable = false;
	fileBeingPrinted.Close();
	fileToPrint.Close();
	fileBeingWritten = NULL;
	doingFileMacro = false;
	dwellWaiting = false;
	stackPointer = 0;
	state = GCodeState::normal;
	drivesRelative = true;
	axesRelative = false;
	probeCount = 0;
	cannedCycleMoveCount = 0;
	cannedCycleMoveQueued = false;
	speedFactor = 1.0 / minutesToSeconds;				// default is just to convert from mm/minute to mm/second
	for (size_t i = 0; i < DRIVES - AXES; ++i)
	{
		extrusionFactors[i] = 1.0;
	}
	for (size_t i = 0; i < DRIVES; ++i)
	{
		moveBuffer.coords[i] = 0.0;
		pausedMoveBuffer[i] = 0.0;
	}
	feedRate = pausedFeedRate = DEFAULT_FEEDRATE/minutesToSeconds;
	ClearMove();

	for (size_t i = 0; i < MaxTriggers; ++i)
	{
		triggers[i].Init();
	}
	triggersPending = 0;

	auxDetected = false;
	while (auxGCodeReply != nullptr)
	{
		auxGCodeReply = OutputBuffer::Release(auxGCodeReply);
	}
	auxSeq = 0;
	simulationMode = 0;
	simulationTime = 0.0;
	isPaused = false;
	filePos = moveBuffer.filePos = noFilePosition;
	lastEndstopStates = platform->GetAllEndstopStates();
	firmwareUpdateModuleMap = 0;
}

float GCodes::FractionOfFilePrinted() const
{
	if (isPaused)
	{
		return (fileToPrint.IsLive()) ? fileToPrint.FractionRead() : -1.0;
	}
	if (stackPointer == 0)
	{
		return (fileBeingPrinted.IsLive() && !doingFileMacro) ? fileBeingPrinted.FractionRead() : -1.0;
	}
	return (stack[0].fileState.IsLive() && !stack[0].doingFileMacro) ? stack[0].fileState.FractionRead() : -1.0;
}

void GCodes::DoFilePrint(GCodeBuffer* gb, StringRef& reply)
{
	if (gb != fileGCode || !isPaused)
	{
		for (int i = 0; i < 50 && fileBeingPrinted.IsLive(); ++i)
		{
			char b;
			if (fileBeingPrinted.Read(b))
			{
				if (gb->StartingNewCode() && gb == fileGCode)
				{
					filePos = fileBeingPrinted.GetPosition() - 1;
					//debugPrintf("Set file pos %u\n", filePos);
				}
				if (gb->Put(b))
				{
					gb->SetFinished(ActOnCode(gb, reply));
					break;
				}
			}
			else
			{
				// We have reached the end of the file.
				// Don't close the file until all moves have been completed, in case the print gets paused.
				// Also, this keeps the state as 'Printing' until the print really has finished.
				if (!gb->StartingNewCode())		// if there is something in the buffer
				{
					if (gb->Put('\n')) 			// in case there wasn't one ending the file
					{
						gb->SetFinished(ActOnCode(gb, reply));
					}
					else
					{
						gb->Init();
					}
				}
				else if (AllMovesAreFinishedAndMoveBufferIsLoaded())
				{
					fileBeingPrinted.Close();
					if (gb == fileGCode)
					{
						reprap.GetPrintMonitor()->StoppedPrint();
						if (platform->Emulating() == marlin)
						{
							// Pronterface expects a "Done printing" message
							HandleReply(gb, false, "Done printing file");
						}
					}
				}
				break;
			}
		}
	}
}

void GCodes::Spin()
{
	if (!active)
		return;

	// First check for new gcodes from all sources except file
	FillGCodeBuffers();

	char replyBuffer[gcodeReplyLength];
	StringRef reply(replyBuffer, ARRAY_SIZE(replyBuffer));
	reply.Clear();

	// Check for M105 poll requests from Pronterface and PanelDue so that the status is kept up to date during execution of file macros etc.
	if (serialGCode->IsReady() && serialGCode->IsPollRequest())
	{
		serialGCode->SetFinished(ActOnCode(serialGCode, reply));
	}
	else if (auxGCode->IsReady() && auxGCode->IsPollRequest())
	{
		auxGCode->SetFinished(ActOnCode(auxGCode, reply));
	}
	else
	{
		// Perform the next operation of the state machine
		// Note: if we change the state to 'normal' from another state, we must call HandleReply to tell the host about the command we have just completed.
		switch (state)
		{
		case GCodeState::normal:
			StartNextGCode(reply);
			break;

		case GCodeState::waitingForMoveToComplete:
			if (AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				HandleReply(gbCurrent, false, "");
				state = GCodeState::normal;
			}
			break;

		case GCodeState::homing:
			if (toBeHomed & (1 << X_AXIS))
			{
				toBeHomed &= ~(1 << X_AXIS);
				DoFileMacro(HOME_X_G);
			}
			else if (toBeHomed & (1 << Y_AXIS))
			{
				toBeHomed &= ~(1 << Y_AXIS);
				DoFileMacro(HOME_Y_G);
			}
			else if (toBeHomed & (1 << Z_AXIS))
			{
				toBeHomed &= ~(1 << Z_AXIS);
				DoFileMacro(HOME_Z_G);
			}
			else
			{
				HandleReply(gbCurrent, false, "");
				state = GCodeState::normal;
			}
			break;

		case GCodeState::setBed1:
			reprap.GetMove()->SetIdentityTransform();
			probeCount = 0;
			state = GCodeState::setBed2;
			// no break

		case GCodeState::setBed2:
			{
				int numProbePoints = reprap.GetMove()->NumberOfXYProbePoints();
				if (DoSingleZProbeAtPoint(probeCount, 0.0))
				{
					probeCount++;
					if (probeCount >= numProbePoints)
					{
						zProbesSet = true;
						reprap.GetMove()->FinishedBedProbing(0, reply);
						HandleReply(gbCurrent, false, reply.Pointer());
						state = GCodeState::normal;
					}
				}
			}
			break;

		case GCodeState::toolChange1: // Release the old tool (if any)
			{
				const Tool *oldTool = reprap.GetCurrentTool();
				if (oldTool != NULL)
				{
					reprap.StandbyTool(oldTool->Number());
				}
			}
			state = GCodeState::toolChange2;
			if (reprap.GetTool(newToolNumber) != nullptr && AllAxesAreHomed())
			{
				scratchString.printf("tpre%d.g", newToolNumber);
				DoFileMacro(scratchString.Pointer(), false);
			}
			break;

		case GCodeState::toolChange2: // Select the new tool (even if it doesn't exist - that just deselects all tools)
			reprap.SelectTool(newToolNumber);
			state = GCodeState::toolChange3;
			if (reprap.GetTool(newToolNumber) != nullptr && AllAxesAreHomed())
			{
				scratchString.printf("tpost%d.g", newToolNumber);
				DoFileMacro(scratchString.Pointer(), false);
			}
			break;

		case GCodeState::toolChange3:
			HandleReply(gbCurrent, false, "");
			state = GCodeState::normal;
			break;

		case GCodeState::pausing1:
			if (AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				state = GCodeState::pausing2;
				DoFileMacro(PAUSE_G);
			}
			break;

		case GCodeState::pausing2:
			HandleReply(gbCurrent, false, "Printing paused");
			state = GCodeState::normal;
			break;

		case GCodeState::resuming1:
		case GCodeState::resuming2:
			// Here when we have just finished running the resume macro file.
			// Move the head back to the paused location
			if (AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				float currentZ = moveBuffer.coords[Z_AXIS];
				for (size_t drive = 0; drive < AXES; ++drive)
				{
					moveBuffer.coords[drive] =  pausedMoveBuffer[drive];
				}
				for (size_t drive = AXES; drive < DRIVES; ++drive)
				{
					moveBuffer.coords[drive] = 0.0;
				}
				moveBuffer.feedRate = DEFAULT_FEEDRATE/minutesToSeconds;	// ask for a good feed rate, we may have paused during a slow move
				moveBuffer.moveType = 0;
				moveBuffer.endStopsToCheck = 0;
				moveBuffer.usePressureAdvance = false;
				moveBuffer.filePos = noFilePosition;
				if (state == GCodeState::resuming1 && currentZ > pausedMoveBuffer[Z_AXIS])
				{
					// First move the head to the correct XY point, then move it down in a separate move
					moveBuffer.coords[Z_AXIS] = currentZ;
					state = GCodeState::resuming2;
				}
				else
				{
					// Just move to the saved position in one go
					state = GCodeState::resuming3;
				}
				moveAvailable = true;
			}
			break;

		case GCodeState::resuming3:
			if (AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				for (size_t i = 0; i < NUM_FANS; ++i)
				{
					platform->SetFanValue(i, pausedFanValues[i]);
				}
				for (size_t drive = AXES; drive < DRIVES; ++drive)
				{
					lastRawExtruderPosition[drive - AXES] = pausedMoveBuffer[drive];	// reset the extruder position in case we are receiving absolute extruder moves
				}
				feedRate = pausedFeedRate;
				isPaused = false;
				HandleReply(gbCurrent, false, "Printing resumed");
				state = GCodeState::normal;
			}
			break;

		case GCodeState::flashing1:
	#ifdef DUET_NG
			// Update additional modules before the main firmware
			if (FirmwareUpdater::IsReady())
			{
				bool updating = false;
				for (unsigned int module = 1; module < NumFirmwareUpdateModules; ++module)
				{
					if ((firmwareUpdateModuleMap & (1 << module)) != 0)
					{
						firmwareUpdateModuleMap &= ~(1 << module);
						FirmwareUpdater::UpdateModule(module);
						updating = true;
						break;
					}
				}
				if (!updating)
				{
					state = GCodeState::flashing2;
				}
			}
	#else
			state = GCodeState::flashing2;
	#endif
			break;

		case GCodeState::flashing2:
			if ((firmwareUpdateModuleMap & 1) != 0)
			{
				// Update main firmware
				firmwareUpdateModuleMap = 0;
				platform->UpdateFirmware();
				// The above call does not return unless an error occurred
			}
			isFlashing = false;
			state = GCodeState::normal;
			break;

		case GCodeState::stopping:		// MO after executing stop.g if present
		case GCodeState::sleeping:		// M1 after executing sleep.g if present
			// Deselect the active tool and turn off all heaters, unless parameter Hn was used with n > 0
			if (!gbCurrent->Seen('H') || gbCurrent->GetIValue() <= 0)
			{
				Tool* tool = reprap.GetCurrentTool();
				if (tool != nullptr)
				{
					reprap.StandbyTool(tool->Number());
				}
				reprap.GetHeat()->SwitchOffAll();
			}

			// chrishamm 2014-18-10: Although RRP says M0 is supposed to turn off all drives and heaters,
			// I think M1 is sufficient for this purpose. Leave M0 for a normal reset.
			if (state == GCodeState::sleeping)
			{
				DisableDrives();
			}
			else
			{
				platform->SetDriversIdle();
			}
			HandleReply(gbCurrent, false, "");
			state = GCodeState::normal;
			break;

		default:				// should not happen
			break;
		}
	}

	platform->ClassReport(longWait);
}

// Get new data into the gcode buffers except the file and macro gcode buffers, and deal with any file uploading
void GCodes::FillGCodeBuffers()
{
	// Webserver
	if (httpGCode->IsIdle())
	{
		for (unsigned int i = 0; i < 16 && webserver->GCodeAvailable(WebSource::HTTP); ++i)
		{
			char b = webserver->ReadGCode(WebSource::HTTP);
			if (httpGCode->Put(b))
			{
				// We have a complete gcode
				if (httpGCode->WritingFileDirectory() != nullptr)
				{
					WriteGCodeToFile(httpGCode);
					httpGCode->SetFinished(true);
				}
				break;
			}
		}
	}

	// Telnet
	if (telnetGCode->IsIdle())
	{
		for (unsigned int i = 0; i < GCODE_LENGTH && webserver->GCodeAvailable(WebSource::Telnet); ++i)
		{
			char b = webserver->ReadGCode(WebSource::Telnet);
			if (telnetGCode->Put(b))
			{
				break;
			}
		}
	}

	// USB interface
	if (serialGCode->IsIdle())
	{
		for (unsigned int i = 0; i < 16 && platform->GCodeAvailable(SerialSource::USB); ++i)
		{
			char b = platform->ReadFromSource(SerialSource::USB);
			// Check the special case of uploading the reprap.htm file
			if (serialGCode->WritingFileDirectory() == platform->GetWebDir())
			{
				WriteHTMLToFile(b, serialGCode);
			}
			else if (serialGCode->Put(b))	// add char to buffer and test whether the gcode is complete
			{
				// We have a complete gcode
				if (serialGCode->WritingFileDirectory() != nullptr)
				{
					WriteGCodeToFile(serialGCode);
					serialGCode->SetFinished(true);
				}
				break;
			}
		}
	}

	// Aux serial port (typically PanelDue)
	if (auxGCode->IsIdle())
	{
		for (unsigned int i = 0; i < 16 && platform->GCodeAvailable(SerialSource::AUX); ++i)
		{
			char b = platform->ReadFromSource(SerialSource::AUX);
			if (auxGCode->Put(b))	// add char to buffer and test whether the gcode is complete
			{
				auxDetected = true;
				break;
			}
		}
	}
}

// Start a new gcode, or continue to execute one that has already been started:
// 1. If we're doing a file macro, don't allow anything else to interrupt it
// 2. Continue executing any gcode that we have already started
// 3. Check for external triggers
// 4. If we have a gcode ready from any non-file sources, start executing it
// 5. Else continue a print from file, if one is running
void GCodes::StartNextGCode(StringRef& reply)
{
	// If a file macro is running, we don't allow anything to interrupt it
	if (doingFileMacro)
	{
		// Complete the current move (must do this before checking whether we have finished the file in case it didn't end in newline)
		if (fileMacroGCode->IsReady() || fileMacroGCode->IsExecuting())
		{
			fileMacroGCode->SetFinished(ActOnCode(fileMacroGCode, reply));
		}
		else if (fileBeingPrinted.IsLive())				// Have we finished the file?
		{
			DoFilePrint(fileMacroGCode, reply);			// No - Do more of the file
		}
		else if (AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			Pop();
			fileMacroGCode->Init();
		}
	}
	// Check for gcodes that we have already started
	else if (httpGCode->IsExecuting())
	{
		httpGCode->SetFinished(ActOnCode(httpGCode, reply));
	}
	else if (telnetGCode->IsExecuting())
	{
		telnetGCode->SetFinished(ActOnCode(telnetGCode, reply));
	}
	else if (serialGCode->IsExecuting())
	{
		serialGCode->SetFinished(ActOnCode(serialGCode, reply));
	}
	else if (auxGCode->IsExecuting())
	{
		auxGCode->SetFinished(ActOnCode(auxGCode, reply));
	}
	else if (fileGCode->IsExecuting())
	{
		fileGCode->SetFinished(ActOnCode(fileGCode, reply));
	}
	// Check triggers
	else if (CheckTriggers())
	{
		// We've handled a trigger, so nothing else to do
	}
	// Check for gcodes we can start
	else if (httpGCode->IsReady())
	{
		httpGCode->SetFinished(ActOnCode(httpGCode, reply));
	}
	else if (telnetGCode->IsReady())
	{
		telnetGCode->SetFinished(ActOnCode(telnetGCode, reply));
	}
	else if (serialGCode->IsReady())
	{
		serialGCode->SetFinished(ActOnCode(serialGCode, reply));
	}
	else if (auxGCode->IsReady())
	{
		auxGCode->SetFinished(ActOnCode(auxGCode, reply));
	}
	// Print some more of the current file
	else
	{
		DoFilePrint(fileGCode, reply);
	}
}

// Check for and execute triggers, returning true if started executing one.
// We already checked that no file macro is being executed before calling this.
bool GCodes::CheckTriggers()
{
	// Check for endstop state changes that activate new triggers
	const TriggerMask oldEndstopStates = lastEndstopStates;
	lastEndstopStates = platform->GetAllEndstopStates();
	const TriggerMask risen = lastEndstopStates & ~oldEndstopStates,
					  fallen = ~lastEndstopStates & oldEndstopStates;
	unsigned int lowestTriggerPending = MaxTriggers;
	for (unsigned int triggerNumber = 0; triggerNumber < MaxTriggers; ++triggerNumber)
	{
		const Trigger& ct = triggers[triggerNumber];
		if (   ((ct.rising & risen) != 0 || (ct.falling & fallen) != 0)
			&& (ct.condition == 0 || (ct.condition == 1 && reprap.GetPrintMonitor()->IsPrinting()))
		   )
		{
			triggersPending |= (1 << triggerNumber);
		}
		if (triggerNumber < lowestTriggerPending && (triggersPending & (1 << triggerNumber)) != 0)
		{
			lowestTriggerPending = triggerNumber;
		}
	}

	// If any triggers are pending, activate the one with the lowest number
	if (lowestTriggerPending < MaxTriggers)
	{
		gbCurrent = nullptr;
		triggersPending &= ~(1 << lowestTriggerPending);		// clear the trigger

		// Execute the trigger
		switch(lowestTriggerPending)
		{
		case 0:
			// Trigger 0 does an emergency stop
			DoEmergencyStop();
			break;

		case 1:
			// Trigger 1 pauses the print, if printing from file
			if (!isPaused && reprap.GetPrintMonitor()->IsPrinting())
			{
				DoPause(true);
			}
			break;

		default:
			// All other trigger numbers execute the corresponding macro file
			{
				char buffer[25];
				StringRef filename(buffer, ARRAY_SIZE(buffer));
				filename.printf(SYS_DIR "trigger%u.g", lowestTriggerPending);
				DoFileMacro(filename.Pointer(), true);
			}
		}
		return true;		// we processed a trigger
	}
	return false;			// no triggers were pending
}

// Execute an emergency stop
void GCodes::DoEmergencyStop()
{
	reprap.EmergencyStop();
	Reset();
	platform->Message(GENERIC_MESSAGE, "Emergency Stop! Reset the controller to continue.");
}

// Pause the print. Before calling this, check that we are doing a file print that isn't already paused.
void GCodes::DoPause(bool externalToFile)
{
	if (externalToFile)
	{
		// Pausing a file print via another input source
		pausedFeedRate = feedRate;										// the call to PausePrint may or may not change this
		FilePosition fPos = reprap.GetMove()->PausePrint(pausedMoveBuffer, pausedFeedRate);	// tell Move we wish to pause the current print
		FileData& fdata = (stackPointer == 0) ? fileBeingPrinted : stack[0].fileState;
		if (fPos != noFilePosition && fdata.IsLive())
		{
			fdata.Seek(fPos);											// replay the abandoned instructions if/when we resume
		}
		fileGCode->Init();
		if (moveAvailable)
		{
			for (size_t drive = AXES; drive < DRIVES; ++drive)
			{
				pausedMoveBuffer[drive] += moveBuffer.coords[drive];	// add on the extrusion in the move not yet taken
			}
			ClearMove();
		}

		for (size_t drive = AXES; drive < DRIVES; ++drive)
		{
			pausedMoveBuffer[drive] = lastRawExtruderPosition[drive - AXES] - pausedMoveBuffer[drive];
		}

		if (reprap.Debug(moduleGcodes))
		{
			platform->MessageF(GENERIC_MESSAGE, "Paused print, file offset=%u\n", fPos);
		}
	}
	else
	{
		// Pausing a file print because of a command in the file itself
		for (size_t drive = 0; drive < AXES; ++drive)
		{
			pausedMoveBuffer[drive] = moveBuffer.coords[drive];
		}
		for (size_t drive = AXES; drive < DRIVES; ++drive)
		{
			pausedMoveBuffer[drive] = lastRawExtruderPosition[drive - AXES];	// get current extruder positions into pausedMoveBuffer
		}
		pausedFeedRate = feedRate;
	}

	for (size_t i = 0; i < NUM_FANS; ++i)
	{
		pausedFanValues[i] = platform->GetFanValue(i);
	}
	state = GCodeState::pausing1;
	isPaused = true;
}

void GCodes::Diagnostics(MessageType mtype)
{
	platform->Message(mtype, "GCodes Diagnostics:\n");
	platform->MessageF(mtype, "Move available? %s\n", moveAvailable ? "yes" : "no");
	platform->MessageF(mtype, "Stack pointer: %u of %u\n", stackPointer, StackSize);
	fileMacroGCode->Diagnostics(mtype);
	httpGCode->Diagnostics(mtype);
	telnetGCode->Diagnostics(mtype);
	serialGCode->Diagnostics(mtype);
	auxGCode->Diagnostics(mtype);
	fileGCode->Diagnostics(mtype);
}

// The wait till everything's done function.  If you need the machine to
// be idle before you do something (for example homing an axis, or shutting down) call this
// until it returns true.  As a side-effect it loads moveBuffer with the last
// position and feedrate for you.

bool GCodes::AllMovesAreFinishedAndMoveBufferIsLoaded()
{
	// Last one gone?
	if (moveAvailable)
		return false;

	// Wait for all the queued moves to stop so we get the actual last position
	if (!reprap.GetMove()->AllMovesAreFinished())
		return false;

	reprap.GetMove()->ResumeMoving();
	reprap.GetMove()->GetCurrentUserPosition(moveBuffer.coords, 0);
	return true;
}

// Save (some of) the state of the machine for recovery in the future.
void GCodes::Push()
{
	if (stackPointer >= StackSize)
	{
		platform->Message(GENERIC_MESSAGE, "Push(): stack overflow!\n");
		return;
	}

	stack[stackPointer].state = state;
	stack[stackPointer].gb = gbCurrent;
	stack[stackPointer].feedrate = feedRate;
	stack[stackPointer].fileState.CopyFrom(fileBeingPrinted);
	stack[stackPointer].drivesRelative = drivesRelative;
	stack[stackPointer].axesRelative = axesRelative;
	stack[stackPointer].doingFileMacro = doingFileMacro;
	stackPointer++;
}

// Recover a saved state
void GCodes::Pop()
{
	if (stackPointer < 1)
	{
		platform->Message(GENERIC_MESSAGE, "Pop(): stack underflow!\n");
		return;
	}

	stackPointer--;
	state = stack[stackPointer].state;
	gbCurrent = stack[stackPointer].gb;
	feedRate = stack[stackPointer].feedrate;
	fileBeingPrinted.MoveFrom(stack[stackPointer].fileState);
	drivesRelative = stack[stackPointer].drivesRelative;
	axesRelative = stack[stackPointer].axesRelative;
	doingFileMacro = stack[stackPointer].doingFileMacro;
}

// Move expects all axis movements to be absolute, and all
// extruder drive moves to be relative.  This function serves that.
// If applyLimits is true and we have homed the relevant axes, then we don't allow movement beyond the bed.
// Returns true if we have a legal move (or G92 argument), false if this gcode should be discarded
bool GCodes::LoadMoveBufferFromGCode(GCodeBuffer *gb, bool doingG92, bool applyLimits)
{
	// Zero every extruder drive as some drives may not be changed
	for (size_t drive = AXES; drive < DRIVES; drive++)
	{
		moveBuffer.coords[drive] = 0.0;
	}

	// Deal with feed rate
	if (gb->Seen(feedrateLetter))
	{
		feedRate = gb->GetFValue() * distanceScale * speedFactor;
	}
	moveBuffer.feedRate = feedRate;

	// First do extrusion, and check, if we are extruding, that we have a tool to extrude with
	Tool* tool = reprap.GetCurrentTool();
	if (gb->Seen(extrudeLetter))
	{
		if (tool == nullptr)
		{
			platform->Message(GENERIC_MESSAGE, "Attempting to extrude with no tool selected.\n");
			return false;
		}
		size_t eMoveCount = tool->DriveCount();
		if (eMoveCount > 0)
		{
			float eMovement[DRIVES - AXES];
			if (tool->GetMixing())
			{
				float length = gb->GetFValue();
				for (size_t drive = 0; drive < tool->DriveCount(); drive++)
				{
					eMovement[drive] = length * tool->GetMix()[drive];
				}
			}
			else
			{
				size_t mc = eMoveCount;
				gb->GetFloatArray(eMovement, mc, false);
				if (eMoveCount != mc)
				{
					platform->MessageF(GENERIC_MESSAGE, "Wrong number of extruder drives for the selected tool: %s\n", gb->Buffer());
					return false;
				}
			}

			// Set the drive values for this tool.
			// chrishamm-2014-10-03: Do NOT check extruder temperatures here, because we may be executing queued codes like M116
			for (size_t eDrive = 0; eDrive < eMoveCount; eDrive++)
			{
				int drive = tool->Drive(eDrive);
				float moveArg = eMovement[eDrive] * distanceScale;
				if (doingG92)
				{
					moveBuffer.coords[drive + AXES] = moveArg;
					lastRawExtruderPosition[drive] = moveArg;
				}
				else
				{
					float extrusionAmount = (drivesRelative)
												? moveArg
												: moveArg - lastRawExtruderPosition[drive];
					lastRawExtruderPosition[drive] += extrusionAmount;
					rawExtruderTotalByDrive[drive] += extrusionAmount;
					rawExtruderTotal += extrusionAmount;
					moveBuffer.coords[drive + AXES] = extrusionAmount * extrusionFactors[drive];
				}
			}
		}
	}

	// Now the movement axes
	const Tool *currentTool = reprap.GetCurrentTool();
	for (size_t axis = 0; axis < AXES; axis++)
	{
		if (gb->Seen(axisLetters[axis]))
		{
			float moveArg = gb->GetFValue() * distanceScale * axisScaleFactors[axis];
			if (doingG92)
			{
				axisIsHomed[axis] = true;		// doing a G92 defines the absolute axis position
			}
			else
			{
				if (axesRelative)
				{
					moveArg += moveBuffer.coords[axis];
				}
				else if (currentTool != NULL)
				{
					moveArg -= currentTool->GetOffset()[axis];// adjust requested position to compensate for tool offset
				}

				// If on a Cartesian printer and applying limits, limit all axes
				if (applyLimits && axisIsHomed[axis] && !reprap.GetMove()->IsDeltaMode()
#if SUPPORT_ROLAND
						&& !reprap.GetRoland()->Active()
#endif
					)
				{
					if (moveArg < platform->AxisMinimum(axis))
					{
						moveArg = platform->AxisMinimum(axis);
					}
					else if (moveArg > platform->AxisMaximum(axis))
					{
						moveArg = platform->AxisMaximum(axis);
					}
				}
			}
			moveBuffer.coords[axis] = moveArg;
		}
	}

	// If axes have been homed on a delta printer and this isn't a homing move, check for movements outside limits.
	// Skip this check if axes have not been homed, so that extruder-only moved are allowed before homing
	if (applyLimits && reprap.GetMove()->IsDeltaMode() && AllAxesAreHomed())
	{
		// Constrain the move to be within the build radius
		float diagonalSquared = fsquare(moveBuffer.coords[X_AXIS]) + fsquare(moveBuffer.coords[Y_AXIS]);
		if (diagonalSquared > reprap.GetMove()->GetDeltaParams().GetPrintRadiusSquared())
		{
			float factor = sqrtf(reprap.GetMove()->GetDeltaParams().GetPrintRadiusSquared() / diagonalSquared);
			moveBuffer.coords[X_AXIS] *= factor;
			moveBuffer.coords[Y_AXIS] *= factor;
		}

		// Constrain the end height of the move to be no greater than the homed height and no lower than -0.2mm
		moveBuffer.coords[Z_AXIS] = max<float>(platform->AxisMinimum(Z_AXIS),
				min<float>(moveBuffer.coords[Z_AXIS], reprap.GetMove()->GetDeltaParams().GetHomedHeight()));
	}

	return true;
}

// This function is called for a G Code that makes a move.
// If the Move class can't receive the move (i.e. things have to wait), return 0.
// If we have queued the move and the caller doesn't need to wait for it to complete, return 1.
// If we need to wait for the move to complete before doing another one (e.g. because endstops are checked in this move), return 2.

int GCodes::SetUpMove(GCodeBuffer *gb, StringRef& reply)
{
	// Last one gone yet?
	if (moveAvailable)
	{
		return 0;
	}

	// Check to see if the move is a 'homing' move that endstops are checked on.
	moveBuffer.endStopsToCheck = 0;
	moveBuffer.moveType = 0;
	if (gb->Seen('S'))
	{
		int ival = gb->GetIValue();
		if (ival == 1 || ival == 2)
		{
			moveBuffer.moveType = ival;
		}

		if (ival == 1)
		{
			for (size_t i = 0; i < AXES; ++i)
			{
				if (gb->Seen(axisLetters[i]))
				{
					moveBuffer.endStopsToCheck |= (1 << i);
				}
			}
		}
	}

	if (reprap.GetMove()->IsDeltaMode())
	{
		// Extra checks to avoid damaging delta printers
		if (moveBuffer.moveType != 0 && !axesRelative)
		{
			// We have been asked to do a move without delta mapping on a delta machine, but the move is not relative.
			// This may be damaging and is almost certainly a user mistake, so ignore the move.
			reply.copy("Attempt to move the motors of a delta printer to absolute positions");
			return 1;
		}

		if (moveBuffer.moveType == 0 && !AllAxesAreHomed())
		{
			// The user may be attempting to move a delta printer to an XYZ position before homing the axes
			// This may be damaging and is almost certainly a user mistake, so ignore the move. But allow extruder-only moves.
			if (gb->Seen(axisLetters[X_AXIS]) || gb->Seen(axisLetters[Y_AXIS]) || gb->Seen(axisLetters[Z_AXIS]))
			{
				reply.copy("Attempt to move the head of a delta printer before homing the towers");
				return 1;
			}
		}
	}

	// Load the last position and feed rate into moveBuffer
#if SUPPORT_ROLAND
	if (reprap.GetRoland()->Active())
	{
		reprap.GetRoland()->GetCurrentRolandPosition(moveBuffer);
	}
	else
#endif
	{
		reprap.GetMove()->GetCurrentUserPosition(moveBuffer.coords, moveBuffer.moveType);
	}

	// Load the move buffer with either the absolute movement required or the relative movement required
	const float currentX = moveBuffer.coords[X_AXIS];
	const float currentY = moveBuffer.coords[Y_AXIS];
	moveAvailable = LoadMoveBufferFromGCode(gb, false, limitAxes && moveBuffer.moveType == 0);
	if (moveAvailable)
	{
		// Flag whether we should use pressure advance, if there is any extrusion in this move.
		// We assume it is a normal printing move needing pressure advance if there is forward extrusion and XY movement.
		// The movement code will only apply pressure advance if there is forward extrusion, so we only need to check for XY movement here.
		moveBuffer.usePressureAdvance = (moveBuffer.coords[X_AXIS] != currentX || moveBuffer.coords[Y_AXIS] != currentY);
		moveBuffer.filePos = (gb == fileGCode) ? filePos : noFilePosition;
		//debugPrintf("Queue move pos %u\n", moveFilePos);
	}
	return (moveBuffer.moveType != 0) ? 2 : 1;
}

// The Move class calls this function to find what to do next.

bool GCodes::ReadMove(RawMove& m)
{
	if (!moveAvailable)
	{
		return false;
	}

	m = moveBuffer;
	ClearMove();
	return true;
}

void GCodes::ClearMove()
{
	moveAvailable = false;
	moveBuffer.endStopsToCheck = 0;
	moveBuffer.moveType = 0;
	moveBuffer.isFirmwareRetraction = false;
}

// Run a file macro. Prior to calling this, 'state' must be set to the state we want to enter when the macro has been completed.
// Return true if the file was found or it wasn't and we were asked to report that fact.
bool GCodes::DoFileMacro(const char* fileName, bool reportMissing)
{
	FileStore *f = platform->GetFileStore(platform->GetSysDir(), fileName, false);
	if (f == nullptr)
	{
		if (reportMissing)
		{
			// Don't use snprintf into scratchString here, because fileName may be aliased to scratchString
			platform->MessageF(GENERIC_MESSAGE, "Macro file %s not found.\n", fileName);
			return true;
		}
		return false;
	}

	Push();
	fileBeingPrinted.Set(f);
	doingFileMacro = true;
	fileMacroGCode->Init();
	state = GCodeState::normal;
	return true;
}

void GCodes::FileMacroCyclesReturn()
{
	if (doingFileMacro)
	{
		Pop();
		fileMacroGCode->Init();
	}
}

// To execute any move, call this until it returns true.
// moveToDo[] entries corresponding with false entries in action[] will
// be ignored.  Recall that moveToDo[DRIVES] should contain the feedrate
// you want (if action[DRIVES] is true).
bool GCodes::DoCannedCycleMove(EndstopChecks ce)
{
	if (AllMovesAreFinishedAndMoveBufferIsLoaded())
	{
		if (cannedCycleMoveQueued)		// if the move has already been queued, it must have finished
		{
			Pop();
			cannedCycleMoveQueued = false;
			return true;
		}

		// Otherwise, the move has not been queued yet
		Push();

		for (size_t drive = 0; drive < DRIVES; drive++)
		{
			if (activeDrive[drive])
			{
				moveBuffer.coords[drive] = moveToDo[drive];
			}
		}
		moveBuffer.feedRate = moveToDo[DRIVES];
		moveBuffer.endStopsToCheck = ce;
		moveBuffer.filePos = noFilePosition;
		moveBuffer.usePressureAdvance = false;
		moveAvailable = true;
		cannedCycleMoveQueued = true;
	}
	return false;
}

// This sets positions.  I.e. it handles G92.
bool GCodes::SetPositions(GCodeBuffer *gb)
{
	// Don't pause the machine if only extruder drives are being reset (DC, 2015-09-06).
	// This avoids blobs and seams when the gcode uses absolute E coordinates and periodically includes G92 E0.
	bool includingAxes = false;
	for (size_t drive = 0; drive < AXES; ++drive)
	{
		if (gb->Seen(axisLetters[drive]))
		{
			includingAxes = true;
			break;
		}
	}

	if (includingAxes)
	{
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
	}
	else if (moveAvailable)			// wait for previous move to be taken so that GetCurrentUserPosition returns the correct value
	{
		return false;
	}

	reprap.GetMove()->GetCurrentUserPosition(moveBuffer.coords, 0);		// make sure move buffer is up to date
	bool ok = LoadMoveBufferFromGCode(gb, true, false);
	if (ok && includingAxes)
	{
#if SUPPORT_ROLAND
		if (reprap.GetRoland()->Active())
		{
			for(size_t axis = 0; axis < AXES; axis++)
			{
				if (!reprap.GetRoland()->ProcessG92(moveBuffer[axis], axis))
				{
					return false;
				}
			}
		}
#endif
		SetPositions(moveBuffer.coords);
	}

	return true;
}

// Offset the axes by the X, Y, and Z amounts in the M code in gb.  Say the machine is at [10, 20, 30] and
// the offsets specified are [8, 2, -5].  The machine will move to [18, 22, 25] and henceforth consider that point
// to be [10, 20, 30].
bool GCodes::OffsetAxes(GCodeBuffer* gb)
{
	if (!offSetSet)
	{
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		for (size_t drive = 0; drive < DRIVES; drive++)
		{
			if (drive < AXES)
			{
				record[drive] = moveBuffer.coords[drive];
				moveToDo[drive] = moveBuffer.coords[drive];
			}
			else
			{
				record[drive] = 0.0;
				moveToDo[drive] = 0.0;
			}
			activeDrive[drive] = false;
		}

		for (size_t axis = 0; axis < AXES; axis++)
		{
			if (gb->Seen(axisLetters[axis]))
			{
				moveToDo[axis] += gb->GetFValue();
				activeDrive[axis] = true;
			}
		}

		if (gb->Seen(feedrateLetter)) // Has the user specified a feedrate?
		{
			moveToDo[DRIVES] = gb->GetFValue() * distanceScale * SECONDS_TO_MINUTES;
		}
		else
		{
			moveToDo[DRIVES] = feedRate;
		}

		offSetSet = true;
	}

	if (DoCannedCycleMove(0))
	{
		// Restore positions
		for (size_t drive = 0; drive < DRIVES; drive++)
		{
			moveBuffer.coords[drive] = record[drive];
		}
		reprap.GetMove()->SetLiveCoordinates(record);	// This doesn't transform record
		reprap.GetMove()->SetPositions(record);			// This does
		offSetSet = false;
		return true;
	}

	return false;
}

// Home one or more of the axes.  Which ones are decided by the
// booleans homeX, homeY and homeZ.
// Returns true if completed, false if needs to be called again.
// 'reply' is only written if there is an error.
// 'error' is false on entry, gets changed to true if there is an error.
bool GCodes::DoHome(GCodeBuffer *gb, StringRef& reply, bool& error)
{
	if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
	{
		return false;
	}

#if SUPPORT_ROLAND
	// Deal with a Roland configuration
	if (reprap.GetRoland()->Active())
	{
		bool rolHome = reprap.GetRoland()->ProcessHome();
		if (rolHome)
		{
			for(size_t axis = 0; axis < AXES; axis++)
			{
				axisIsHomed[axis] = true;
			}
		}
		return rolHome;
	}
#endif

	if (reprap.GetMove()->IsDeltaMode())
	{
		SetAllAxesNotHomed();
		DoFileMacro(HOME_DELTA_G);
	}
	else
	{
		toBeHomed = 0;
		for (size_t axis = 0; axis < AXES; ++axis)
		{
			if (gb->Seen(axisLetters[axis]))
			{
				toBeHomed |= (1 << axis);
				axisIsHomed[axis] = false;
			}
		}

		if (toBeHomed == 0 || toBeHomed == ((1 << X_AXIS) | (1 << Y_AXIS) | (1 << Z_AXIS)))
		{
			// Homing everything
			SetAllAxesNotHomed();
			DoFileMacro(HOME_ALL_G);
		}
		else if (   platform->MustHomeXYBeforeZ()
				 && ((toBeHomed & (1 << Z_AXIS)) != 0)
				 && (   (((toBeHomed & (1 << X_AXIS)) == 0) && !axisIsHomed[X_AXIS])
					 || (((toBeHomed & (1 << Y_AXIS)) == 0) && !axisIsHomed[Y_AXIS])
					)
				)
		{
			// We can only home Z if both X and Y have already been homed or are being homed
			reply.copy("Must home X and Y before homing Z");
			error = true;
		}
		else
		{
			state = GCodeState::homing;
		}
	}
	return true;
}

// This lifts Z a bit, moves to the probe XY coordinates (obtained by a call to GetProbeCoordinates() ),
// probes the bed height, and records the Z coordinate probed.  If you want to program any general
// internal canned cycle, this shows how to do it.
// On entry, probePointIndex specifies which of the points this is.
bool GCodes::DoSingleZProbeAtPoint(int probePointIndex, float heightAdjust)
{
	reprap.GetMove()->SetIdentityTransform(); 		// It doesn't matter if these are called repeatedly

	for (size_t drive = 0; drive <= DRIVES; drive++)
	{
		activeDrive[drive] = false;
	}

	switch (cannedCycleMoveCount)
	{
	case 0: // Move Z to the dive height. This only does anything on the first move; on all the others Z is already there
		moveToDo[Z_AXIS] = platform->GetZProbeDiveHeight() + max<float>(platform->ZProbeStopHeight(), 0.0);
		activeDrive[Z_AXIS] = true;
		moveToDo[DRIVES] = platform->GetZProbeTravelSpeed();
		if (DoCannedCycleMove(0))
		{
			cannedCycleMoveCount++;
		}
		return false;

	case 1:	// Move to the correct XY coordinates
		GetProbeCoordinates(probePointIndex, moveToDo[X_AXIS], moveToDo[Y_AXIS], moveToDo[Z_AXIS]);
		activeDrive[X_AXIS] = true;
		activeDrive[Y_AXIS] = true;
		// NB - we don't use the Z value
		moveToDo[DRIVES] = platform->GetZProbeTravelSpeed();
		if (DoCannedCycleMove(0))
		{
			cannedCycleMoveCount++;
		}
		return false;

	case 2:	// Probe the bed
		{
			const float height = (axisIsHomed[Z_AXIS])
									? 2 * platform->GetZProbeDiveHeight()			// Z axis has been homed, so no point in going very far
									: 1.1 * platform->AxisTotalLength(Z_AXIS);		// Z axis not homed yet, so treat this as a homing move
			switch(DoZProbe(height))
			{
			case 0:
				// Z probe is already triggered at the start of the move, so abandon the probe and record an error
				platform->Message(GENERIC_MESSAGE, "Z probe warning: probe already triggered at start of probing move\n");
				cannedCycleMoveCount++;
				reprap.GetMove()->SetZBedProbePoint(probePointIndex, platform->GetZProbeDiveHeight(), true, true);
				break;

			case 1:
				if (axisIsHomed[Z_AXIS])
				{
					lastProbedZ = moveBuffer.coords[Z_AXIS] - (platform->ZProbeStopHeight() + heightAdjust);
				}
				else
				{
					// The Z axis has not yet been homed, so treat this probe as a homing move.
					moveBuffer.coords[Z_AXIS] = platform->ZProbeStopHeight() + heightAdjust;
					SetPositions(moveBuffer.coords);
					axisIsHomed[Z_AXIS] = true;
					lastProbedZ = 0.0;
				}
				reprap.GetMove()->SetZBedProbePoint(probePointIndex, lastProbedZ, true, false);
				cannedCycleMoveCount++;
				break;

			default:
				break;
			}
		}
		return false;

	case 3:	// Raise the head back up to the dive height
		moveToDo[Z_AXIS] = platform->GetZProbeDiveHeight() + max<float>(platform->ZProbeStopHeight(), 0.0);
		activeDrive[Z_AXIS] = true;
		moveToDo[DRIVES] = platform->GetZProbeTravelSpeed();
		if (DoCannedCycleMove(0))
		{
			cannedCycleMoveCount = 0;
			return true;
		}
		return false;

	default: // should not happen
		cannedCycleMoveCount = 0;
		return true;
	}
}

// This simply moves down till the Z probe/switch is triggered. Call it repeatedly until it returns true.
// Called when we do a G30 with no P parameter.
bool GCodes::DoSingleZProbe(bool reportOnly, float heightAdjust)
{
	switch (DoZProbe(1.1 * platform->AxisTotalLength(Z_AXIS)))
	{
	case 0:		// failed
		return true;

	case 1:		// success
		if (!reportOnly)
		{
			moveBuffer.coords[Z_AXIS] = platform->ZProbeStopHeight() + heightAdjust;
			SetPositions(moveBuffer.coords);
			axisIsHomed[Z_AXIS] = true;
			lastProbedZ = 0.0;
		}
		return true;

	default:	// not finished yet
		return false;
	}
}

// Do a Z probe cycle up to the maximum specified distance.
// Returns -1 if not complete yet
// Returns 0 if failed
// Returns 1 if success, with lastProbedZ set to the height we stopped at and the current position in moveBuffer
int GCodes::DoZProbe(float distance)
{
	if (platform->GetZProbeType() == ZProbeTypeDelta)
	{
		const ZProbeParameters& params = platform->GetZProbeParameters();
		return reprap.GetMove()->DoDeltaProbe(params.param1, params.param2, params.probeSpeed, distance);
	}
	else
	{
		if (!cannedCycleMoveQueued && reprap.GetPlatform()->GetZProbeResult() == EndStopHit::lowHit)
		{
			return 0;
		}

		// Do a normal canned cycle Z movement with Z probe enabled
		for (size_t drive = 0; drive <= DRIVES; drive++)
		{
			activeDrive[drive] = false;
		}

		moveToDo[Z_AXIS] = -distance;
		activeDrive[Z_AXIS] = true;
		moveToDo[DRIVES] = platform->GetZProbeParameters().probeSpeed;

		if (DoCannedCycleMove(ZProbeActive))
		{
			return 1;
		}
		return -1;
	}
}

// This is called to execute a G30.
// It sets wherever we are as the probe point P (probePointIndex)
// then probes the bed, or gets all its parameters from the arguments.
// If X or Y are specified, use those; otherwise use the machine's
// coordinates.  If no Z is specified use the machine's coordinates.  If it
// is specified and is greater than SILLY_Z_VALUE (i.e. greater than -9999.0)
// then that value is used.  If it's less than SILLY_Z_VALUE the bed is
// probed and that value is used.
// Call this repeatedly until it returns true.
bool GCodes::SetSingleZProbeAtAPosition(GCodeBuffer *gb, StringRef& reply)
{
	if (reprap.GetMove()->IsDeltaMode() && !AllAxesAreHomed())
	{
		reply.copy("Must home before bed probing");
		return true;
	}

	float heightAdjust = 0;
	if (gb->Seen('H'))
	{
		heightAdjust = gb->GetFValue();
	}

	if (!gb->Seen('P'))
	{
		bool reportOnly = false;
		if (gb->Seen('S') && gb->GetIValue() < 0)
		{
			reportOnly = true;
		}
		return DoSingleZProbe(reportOnly, heightAdjust);
	}

	int probePointIndex = gb->GetIValue();
	if (probePointIndex < 0 || (unsigned int)probePointIndex >= MAX_PROBE_POINTS)
	{
		reprap.GetPlatform()->Message(GENERIC_MESSAGE, "Z probe point index out of range.\n");
		return true;
	}

	float x = (gb->Seen(axisLetters[X_AXIS])) ? gb->GetFValue() : moveBuffer.coords[X_AXIS];
	float y = (gb->Seen(axisLetters[Y_AXIS])) ? gb->GetFValue() : moveBuffer.coords[Y_AXIS];
	float z = (gb->Seen(axisLetters[Z_AXIS])) ? gb->GetFValue() : moveBuffer.coords[Z_AXIS];

	reprap.GetMove()->SetXBedProbePoint(probePointIndex, x);
	reprap.GetMove()->SetYBedProbePoint(probePointIndex, y);

	if (z > SILLY_Z_VALUE)
	{
		reprap.GetMove()->SetZBedProbePoint(probePointIndex, z, false, false);
		if (gb->Seen('S'))
		{
			zProbesSet = true;
			reprap.GetMove()->FinishedBedProbing(gb->GetIValue(), reply);
		}
		return true;
	}
	else
	{
		if (DoSingleZProbeAtPoint(probePointIndex, heightAdjust))
		{
			if (gb->Seen('S'))
			{
				zProbesSet = true;
				int sParam = gb->GetIValue();
				if (sParam == 1)
				{
					// G30 with a silly Z value and S=1 is equivalent to G30 with no parameters in that it sets the current Z height
					// This is useful because it adjusts the XY position to account for the probe offset.
					moveBuffer.coords[Z_AXIS] += lastProbedZ;
					SetPositions(moveBuffer.coords);
					lastProbedZ = 0.0;
				}
				else
				{
					reprap.GetMove()->FinishedBedProbing(sParam, reply);
				}
			}
			return true;
		}
	}

	return false;
}

// This returns the (X, Y) points to probe the bed at probe point count.  When probing, it returns false.
// If called after probing has ended it returns true, and the Z coordinate probed is also returned.
bool GCodes::GetProbeCoordinates(int count, float& x, float& y, float& z) const
{
	const ZProbeParameters& rp = platform->GetZProbeParameters();
	x = reprap.GetMove()->XBedProbePoint(count) - rp.xOffset;
	y = reprap.GetMove()->YBedProbePoint(count) - rp.yOffset;
	z = reprap.GetMove()->ZBedProbePoint(count);
	return zProbesSet;
}

bool GCodes::SetPrintZProbe(GCodeBuffer* gb, StringRef& reply)
{
	ZProbeParameters params = platform->GetZProbeParameters();
	bool seen = false;
	if (gb->Seen(axisLetters[X_AXIS]))
	{
		params.xOffset = gb->GetFValue();
		seen = true;
	}
	if (gb->Seen(axisLetters[Y_AXIS]))
	{
		params.yOffset = gb->GetFValue();
		seen = true;
	}
	if (gb->Seen(axisLetters[Z_AXIS]))
	{
		params.height = gb->GetFValue();
		seen = true;
	}
	if (gb->Seen('P'))
	{
		params.adcValue = gb->GetIValue();
		seen = true;
	}
	if (gb->Seen('C'))
	{
		params.temperatureCoefficient = gb->GetFValue();
		seen = true;
		if (gb->Seen('S'))
		{
			params.calibTemperature = gb->GetFValue();
		}
		else
		{
			// Use the current bed temperature as the calibration temperature if no value was provided
			params.calibTemperature = platform->GetZProbeTemperature();
		}
	}

	if (seen)
	{
		platform->SetZProbeParameters(params);
	}
	else
	{
		int v0 = platform->ZProbe();
		int v1, v2;
		switch (platform->GetZProbeSecondaryValues(v1, v2))
		{
		case 1:
			reply.printf("%d (%d)", v0, v1);
			break;
		case 2:
			reply.printf("%d (%d, %d)", v0, v1, v2);
			break;
		default:
			reply.printf("%d", v0);
			break;
		}
	}
	return true;
}

// Return the current coordinates as a printable string.  Coordinates
// are updated at the end of each movement, so this won't tell you
// where you are mid-movement.

//Fixed to deal with multiple extruders

void GCodes::GetCurrentCoordinates(StringRef& s) const
{
	float liveCoordinates[DRIVES];
	reprap.GetMove()->LiveCoordinates(liveCoordinates);
	const Tool *currentTool = reprap.GetCurrentTool();
	if (currentTool != NULL)
	{
		const float *offset = currentTool->GetOffset();
		for (size_t i = 0; i < AXES; ++i)
		{
			liveCoordinates[i] += offset[i];
		}
	}

	s.printf("X:%.2f Y:%.2f Z:%.2f ", liveCoordinates[X_AXIS], liveCoordinates[Y_AXIS], liveCoordinates[Z_AXIS]);
	for (size_t i = AXES; i < DRIVES; i++)
	{
		s.catf("E%u:%.1f ", i - AXES, liveCoordinates[i]);
	}

	// Print the axis stepper motor positions as Marlin does, as an aid to debugging.
	// Don't bother with the extruder endpoints, they are zero after any non-extruding move.
	s.cat(" Count");
	for (size_t i = 0; i < AXES; ++i)
	{
		s.catf(" %d", reprap.GetMove()->GetEndPoint(i));
	}
}

bool GCodes::OpenFileToWrite(const char* directory, const char* fileName, GCodeBuffer *gb)
{
	fileBeingWritten = platform->GetFileStore(directory, fileName, true);
	eofStringCounter = 0;
	if (fileBeingWritten == NULL)
	{
		platform->MessageF(GENERIC_MESSAGE, "Can't open GCode file \"%s\" for writing.\n", fileName);
		return false;
	}
	else
	{
		gb->SetWritingFileDirectory(directory);
		return true;
	}
}

void GCodes::WriteHTMLToFile(char b, GCodeBuffer *gb)
{
	if (fileBeingWritten == NULL)
	{
		platform->Message(GENERIC_MESSAGE, "Attempt to write to a null file.\n");
		return;
	}

	if (eofStringCounter != 0 && b != eofString[eofStringCounter])
	{
		for (size_t i = 0; i < eofStringCounter; ++i)
		{
			fileBeingWritten->Write(eofString[i]);
		}
		eofStringCounter = 0;
	}

	if (b == eofString[eofStringCounter])
	{
		eofStringCounter++;
		if (eofStringCounter >= eofStringLength)
		{
			fileBeingWritten->Close();
			fileBeingWritten = NULL;
			gb->SetWritingFileDirectory(NULL);
			const char* r = (platform->Emulating() == marlin) ? "Done saving file." : "";
			HandleReply(gb, false, r);
			return;
		}
	}
	else
	{
		fileBeingWritten->Write(b);
	}
}

void GCodes::WriteGCodeToFile(GCodeBuffer *gb)
{
	if (fileBeingWritten == NULL)
	{
		platform->Message(GENERIC_MESSAGE, "Attempt to write to a null file.\n");
		return;
	}

	// End of file?
	if (gb->Seen('M'))
	{
		if (gb->GetIValue() == 29)
		{
			fileBeingWritten->Close();
			fileBeingWritten = NULL;
			gb->SetWritingFileDirectory(NULL);
			const char* r = (platform->Emulating() == marlin) ? "Done saving file." : "";
			HandleReply(gb, false, r);
			return;
		}
	}

	// Resend request?
	if (gb->Seen('G'))
	{
		if (gb->GetIValue() == 998)
		{
			if (gb->Seen('P'))
			{
				scratchString.printf("%d\n", gb->GetIValue());
				HandleReply(gb, false, scratchString.Pointer());
				return;
			}
		}
	}

	fileBeingWritten->Write(gb->Buffer());
	fileBeingWritten->Write('\n');
	HandleReply(gb, false, "");
}

// Set up a file to print, but don't print it yet.
void GCodes::QueueFileToPrint(const char* fileName)
{
	FileStore *f = platform->GetFileStore(platform->GetGCodeDir(), fileName, false);
	if (f != nullptr)
	{
		// Cancel current print if there is any
		if (!reprap.GetPrintMonitor()->IsPrinting())
		{
			CancelPrint();
		}

		fileGCode->SetToolNumberAdjust(0);	// clear tool number adjustment

		// Reset all extruder positions when starting a new print
		for (size_t extruder = AXES; extruder < DRIVES; extruder++)
		{
			lastRawExtruderPosition[extruder - AXES] = 0.0;
			rawExtruderTotalByDrive[extruder - AXES] = 0.0;
		}
		rawExtruderTotal = 0.0;
		reprap.GetMove()->ResetExtruderPositions();

		fileToPrint.Set(f);
	}
	else
	{
		platform->MessageF(GENERIC_MESSAGE, "GCode file \"%s\" not found\n", fileName);
	}
}

void GCodes::DeleteFile(const char* fileName)
{
	if (!platform->GetMassStorage()->Delete(platform->GetGCodeDir(), fileName))
	{
		platform->MessageF(GENERIC_MESSAGE, "Could not delete file \"%s\"\n", fileName);
	}
}

// Function to handle dwell delays.  Return true for dwell finished, false otherwise.
bool GCodes::DoDwell(GCodeBuffer *gb)
{
	if (!gb->Seen('P'))
	{
		return true;  // No time given - throw it away
	}

#if SUPPORT_ROLAND
	// Deal with a Roland configuration
	if (reprap.GetRoland()->Active())
	{
		return reprap.GetRoland()->ProcessDwell(gb->GetLValue());
	}
#endif

	// Wait for all the queued moves to stop
	if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
	{
		return false;
	}

	float dwell = 0.001 * (float) gb->GetLValue(); // P values are in milliseconds; we need seconds

	if (simulationMode != 0)
	{
		simulationTime += dwell;
		return true;
	}
	else
	{
		return DoDwellTime(dwell);
	}
}

bool GCodes::DoDwellTime(float dwell)
{
	// Are we already in the dwell?

	if (dwellWaiting)
	{
		if (platform->Time() - dwellTime >= 0.0)
		{
			dwellWaiting = false;
			reprap.GetMove()->ResumeMoving();
			return true;
		}
		return false;
	}

	// New dwell - set it up

	dwellWaiting = true;
	dwellTime = platform->Time() + dwell;
	return false;
}

// Set offset, working and standby temperatures for a tool. I.e. handle a G10.

void GCodes::SetOrReportOffsets(StringRef& reply, GCodeBuffer *gb)
{
	if (gb->Seen('P'))
	{
		int8_t toolNumber = gb->GetIValue();
		toolNumber += gb->GetToolNumberAdjust();
		Tool* tool = reprap.GetTool(toolNumber);
		if (tool == NULL)
		{
			reply.printf("Attempt to set/report offsets and temperatures for non-existent tool: %d", toolNumber);
			return;
		}

		// Deal with setting offsets
		float offset[AXES];
		for (size_t i = 0; i < AXES; ++i)
		{
			offset[i] = tool->GetOffset()[i];
		}

		bool settingOffset = false;
		if (gb->Seen('X'))
		{
			offset[X_AXIS] = gb->GetFValue();
			settingOffset = true;
		}
		if (gb->Seen('Y'))
		{
			offset[Y_AXIS] = gb->GetFValue();
			settingOffset = true;
		}
		if (gb->Seen('Z'))
		{
			offset[Z_AXIS] = gb->GetFValue();
			settingOffset = true;
		}
		if (settingOffset)
		{
			tool->SetOffset(offset);
		}

		// Deal with setting temperatures
		bool settingTemps = false;
		size_t hCount = tool->HeaterCount();
		float standby[HEATERS];
		float active[HEATERS];
		if (hCount > 0)
		{
			tool->GetVariables(standby, active);
			if (gb->Seen('R'))
			{
				gb->GetFloatArray(standby, hCount, true);
				settingTemps = true;
			}
			if (gb->Seen('S'))
			{
				gb->GetFloatArray(active, hCount, true);
				settingTemps = true;
			}

			if (settingTemps && simulationMode == 0)
			{
				tool->SetVariables(standby, active);
			}
		}

		if (!settingOffset && !settingTemps)
		{
			// Print offsets and temperatures
			reply.printf("Tool %d offsets: X%.1f Y%.1f Z%.1f", toolNumber, offset[X_AXIS], offset[Y_AXIS],
					offset[Z_AXIS]);
			if (hCount != 0)
			{
				reply.cat(", active/standby temperature(s):");
				for (size_t heater = 0; heater < hCount; heater++)
				{
					reply.catf(" %.1f/%.1f", active[heater], standby[heater]);
				}
			}
		}
	}
}

void GCodes::ManageTool(GCodeBuffer *gb, StringRef& reply)
{
	if (!gb->Seen('P'))
	{
		// DC temporary code to allow tool numbers to be adjusted so that we don't need to edit multi-media files generated by slic3r
		if (gb->Seen('S'))
		{
			int adjust = gb->GetIValue();
			gb->SetToolNumberAdjust(adjust);
		}
		return;
	}

	// Check tool number
	bool seen = false;
	int toolNumber = gb->GetLValue();
	if (toolNumber < 0)
	{
		platform->Message(GENERIC_MESSAGE, "Tool number must be positive!\n");
		return;
	}

	// Check drives
	long drives[DRIVES - AXES];  // There can never be more than we have...
	size_t dCount = DRIVES - AXES;  // Sets the limit and returns the count
	if (gb->Seen('D'))
	{
		gb->GetLongArray(drives, dCount);
		seen = true;
	}
	else
	{
		dCount = 0;
	}

	// Check heaters
	long heaters[HEATERS];
	size_t hCount = HEATERS;
	if (gb->Seen('H'))
	{
		gb->GetLongArray(heaters, hCount);
		seen = true;
	}
	else
	{
		hCount = 0;
	}

	if (seen)
	{
		// Add or delete tool, so start by deleting the old one with this number, if any
		reprap.DeleteTool(reprap.GetTool(toolNumber));

		// M563 P# D-1 H-1 removes an existing tool
		if (dCount == 1 && hCount == 1 && drives[0] == -1 && heaters[0] == -1)
		{
			// nothing more to do
		}
		else
		{
			Tool* tool = Tool::Create(toolNumber, drives, dCount, heaters, hCount);
			if (tool != nullptr)
			{
				reprap.AddTool(tool);
			}
		}
	}
	else
	{
		reprap.PrintTool(toolNumber, reply);
	}
}

// Does what it says.
void GCodes::DisableDrives()
{
	for (size_t drive = 0; drive < DRIVES; drive++)
	{
		platform->DisableDrive(drive);
	}
	SetAllAxesNotHomed();
}

// Does what it says.
void GCodes::SetEthernetAddress(GCodeBuffer *gb, int mCode)
{
	byte eth[4];
	const char* ipString = gb->GetString();
	uint8_t sp = 0;
	uint8_t spp = 0;
	uint8_t ipp = 0;
	while (ipString[sp])
	{
		if (ipString[sp] == '.')
		{
			eth[ipp] = atoi(&ipString[spp]);
			ipp++;
			if (ipp > 3)
			{
				platform->MessageF(GENERIC_MESSAGE, "Dud IP address: %s\n", gb->Buffer());
				return;
			}
			sp++;
			spp = sp;
		}
		else
		{
			sp++;
		}
	}
	eth[ipp] = atoi(&ipString[spp]);
	if (ipp == 3)
	{
		switch (mCode)
		{
		case 552:
			platform->SetIPAddress(eth);
			break;
		case 553:
			platform->SetNetMask(eth);
			break;
		case 554:
			platform->SetGateWay(eth);
			break;

		default:
			platform->Message(GENERIC_MESSAGE, "Setting ether parameter - dud code.\n");
		}
	}
	else
	{
		platform->MessageF(GENERIC_MESSAGE, "Dud IP address: %s\n", gb->Buffer());
	}
}

void GCodes::SetMACAddress(GCodeBuffer *gb)
{
	uint8_t mac[6];
	const char* ipString = gb->GetString();
	uint8_t sp = 0;
	uint8_t spp = 0;
	uint8_t ipp = 0;
	while (ipString[sp])
	{
		if (ipString[sp] == ':')
		{
			mac[ipp] = strtoul(&ipString[spp], NULL, 16);
			ipp++;
			if (ipp > 5)
			{
				platform->MessageF(GENERIC_MESSAGE, "Dud MAC address: %s\n", gb->Buffer());
				return;
			}
			sp++;
			spp = sp;
		}
		else
		{
			sp++;
		}
	}
	mac[ipp] = strtoul(&ipString[spp], NULL, 16);
	if (ipp == 5)
	{
		platform->SetMACAddress(mac);
	}
	else
	{
		platform->MessageF(GENERIC_MESSAGE, "Dud MAC address: %s\n", gb->Buffer());
	}
}

bool GCodes::ChangeMicrostepping(size_t drive, int microsteps, int mode) const
{
	bool dummy;
	unsigned int oldSteps = platform->GetMicrostepping(drive, dummy);
	bool success = platform->SetMicrostepping(drive, microsteps, mode);
	if (success && mode <= 1)							// modes higher than 1 are used for special functions
	{
		// We changed the microstepping, so adjust the steps/mm to compensate
		float stepsPerMm = platform->DriveStepsPerUnit(drive);
		if (stepsPerMm > 0)
		{
			platform->SetDriveStepsPerUnit(drive, stepsPerMm * (float)microsteps / (float)oldSteps);
		}
	}
	return success;
}

// Handle sending a reply back to the appropriate interface(s).
// Note that 'reply' may be empty. If it isn't, then we need to append newline when sending it.
// Also, gb may be null if we were executing a trigger macro.
void GCodes::HandleReply(GCodeBuffer *gb, bool error, const char* reply)
{
	// Don't report "ok" responses if a (macro) file is being processed
	// Also check that this response was triggered by a gcode
	if (gb == nullptr || ((gb == fileMacroGCode || gb == fileGCode) && reply[0] == 0))
	{
		return;
	}

	// Second UART device, e.g. dc42's PanelDue. Do NOT use emulation for this one!
	if (gb == auxGCode || (stackPointer != 0 && stack[0].gb == auxGCode))
	{
		// Discard this response if either no aux device is attached or if the response is empty
		if (reply[0] == 0 || !HaveAux())
		{
			return;
		}

		// Regular text-based responses for AUX are always stored and processed by M105/M408
		if (auxGCodeReply == nullptr && !OutputBuffer::Allocate(auxGCodeReply))
		{
			// No more space to buffer this response. Should never happen
			return;
		}
		auxSeq++;
		auxGCodeReply->cat(reply);
		return;
	}

	const Compatibility c = (gb == serialGCode || gb == telnetGCode) ? platform->Emulating() : me;
	MessageType type = GENERIC_MESSAGE;
	if (gb == httpGCode)
	{
		type = HTTP_MESSAGE;
	}
	else if (gb == telnetGCode)
	{
		type = TELNET_MESSAGE;
	}
	else if (gb == serialGCode)
	{
		type = HOST_MESSAGE;
	}

	const char* response = (gb->Seen('M') && gb->GetIValue() == 998) ? "rs " : "ok";
	const char* emulationType = 0;

	switch (c)
	{
		case me:
		case reprapFirmware:
			if (error)
			{
				platform->Message(type, "Error: ");
			}
			platform->Message(type, reply);
			platform->Message(type, "\n");
			return;

		case marlin:
			// We don't need to handle M20 here because we always allocate an output buffer for that one
			if (gb->Seen('M') && gb->GetIValue() == 28)
			{
				platform->Message(type, response);
				platform->Message(type, "\n");
				platform->Message(type, reply);
				platform->Message(type, "\n");
				return;
			}

			if ((gb->Seen('M') && gb->GetIValue() == 105) || (gb->Seen('M') && gb->GetIValue() == 998))
			{
				platform->Message(type, response);
				platform->Message(type, " ");
				platform->Message(type, reply);
				platform->Message(type, "\n");
				return;
			}

			if (reply[0] != 0 && !DoingFileMacro())
			{
				platform->Message(type, reply);
				platform->Message(type, "\n");
				platform->Message(type, response);
				platform->Message(type, "\n");
			}
			else if (reply[0] != 0)
			{
				platform->Message(type, reply);
				platform->Message(type, "\n");
			}
			else
			{
				platform->Message(type, response);
				platform->Message(type, "\n");
			}
			return;

		case teacup:
			emulationType = "teacup";
			break;
		case sprinter:
			emulationType = "sprinter";
			break;
		case repetier:
			emulationType = "repetier";
			break;
		default:
			emulationType = "unknown";
	}

	if (emulationType != 0)
	{
		platform->MessageF(type, "Emulation of %s is not yet supported.\n", emulationType);	// don't send this one to the web as well, it concerns only the USB interface
	}
}

void GCodes::HandleReply(GCodeBuffer *gb, bool error, OutputBuffer *reply)
{
	// Although unlikely, it's possible that we get a nullptr reply. Don't proceed if this is the case
	if (gb == nullptr || reply == nullptr)
	{
		return;
	}

	// Second UART device, e.g. dc42's PanelDue. Do NOT use emulation for this one!
	if (gb == auxGCode || (stackPointer != 0 && stack[0].gb == auxGCode))
	{
		// Discard this response if either no aux device is attached or if the response is empty
		if (reply->Length() == 0 || !HaveAux())
		{
			OutputBuffer::ReleaseAll(reply);
			return;
		}

		// JSON responses are always sent directly to the AUX device
		if ((*reply)[0] == '{')
		{
			platform->Message(AUX_MESSAGE, reply);
			return;
		}

		// Other responses are stored for M105/M408
		auxSeq++;
		if (auxGCodeReply == nullptr)
		{
			auxGCodeReply = reply;
		}
		else
		{
			auxGCodeReply->Append(reply);
		}
		return;
	}

	const Compatibility c = (gb == serialGCode || gb == telnetGCode) ? platform->Emulating() : me;
	MessageType type = GENERIC_MESSAGE;
	if (gb == httpGCode)
	{
		type = HTTP_MESSAGE;
	}
	else if (gb == telnetGCode)
	{
		type = TELNET_MESSAGE;
	}
	else if (gb == serialGCode)
	{
		type = HOST_MESSAGE;
	}

	const char* response = (gb->Seen('M') && gb->GetIValue() == 998) ? "rs " : "ok";
	const char* emulationType = nullptr;

	switch (c)
	{
		case me:
		case reprapFirmware:
			if (error)
			{
				platform->Message(type, "Error: ");
			}
			platform->Message(type, reply);
			return;

		case marlin:
			if (gb->Seen('M') && gb->GetIValue() == 20)
			{
				platform->Message(type, "Begin file list\n");
				platform->Message(type, reply);
				platform->Message(type, "End file list\n");
				platform->Message(type, response);
				platform->Message(type, "\n");
				return;
			}

			if (gb->Seen('M') && gb->GetIValue() == 28)
			{
				platform->Message(type, response);
				platform->Message(type, "\n");
				platform->Message(type, reply);
				return;
			}

			if ((gb->Seen('M') && gb->GetIValue() == 105) || (gb->Seen('M') && gb->GetIValue() == 998))
			{
				platform->Message(type, response);
				platform->Message(type, " ");
				platform->Message(type, reply);
				return;
			}

			if (reply->Length() != 0 && !DoingFileMacro())
			{
				platform->Message(type, reply);
				platform->Message(type, "\n");
				platform->Message(type, response);
				platform->Message(type, "\n");
			}
			else if (reply->Length() != 0)
			{
				platform->Message(type, reply);
			}
			else
			{
				OutputBuffer::ReleaseAll(reply);
				platform->Message(type, response);
				platform->Message(type, "\n");
			}
			return;

		case teacup:
			emulationType = "teacup";
			break;
		case sprinter:
			emulationType = "sprinter";
			break;
		case repetier:
			emulationType = "repetier";
			break;
		default:
			emulationType = "unknown";
	}

	// If we get here then we didn't handle the message, so release the buffer(s)
	OutputBuffer::ReleaseAll(reply);
	if (emulationType != 0)
	{
		platform->MessageF(type, "Emulation of %s is not yet supported.\n", emulationType);	// don't send this one to the web as well, it concerns only the USB interface
	}
}

// Set PID parameters (M301 or M304 command). 'heater' is the default heater number to use.
void GCodes::SetPidParameters(GCodeBuffer *gb, int heater, StringRef& reply)
{
	if (gb->Seen('H'))
	{
		heater = gb->GetIValue();
	}

	if (heater >= 0 && heater < HEATERS)
	{
		PidParameters pp = platform->GetPidParameters(heater);
		bool seen = false;
		if (gb->Seen('P'))
		{
			pp.kP = gb->GetFValue();
			seen = true;
		}
		if (gb->Seen('I'))
		{
			pp.kI = gb->GetFValue();
			seen = true;
		}
		if (gb->Seen('D'))
		{
			pp.kD = gb->GetFValue();
			seen = true;
		}
		if (gb->Seen('T'))
		{
			pp.kT = gb->GetFValue();
			seen = true;
		}
		if (gb->Seen('S'))
		{
			pp.kS = gb->GetFValue();
			seen = true;
		}

		if (seen)
		{
			platform->SetPidParameters(heater, pp);
			reprap.GetHeat()->UseModel(heater, false);
		}
		else
		{
			reply.printf("Heater %d P:%.2f I:%.3f D:%.2f T:%.2f S:%.2f", heater, pp.kP, pp.kI, pp.kD, pp.kT, pp.kS);
		}
	}
}

void GCodes::SetHeaterParameters(GCodeBuffer *gb, StringRef& reply)
{
	if (gb->Seen('P'))
	{
		int heater = gb->GetIValue();
		if (heater >= 0 && heater < HEATERS)
		{
			PidParameters pp = platform->GetPidParameters(heater);
			bool seen = false;

			// We must set the 25C resistance and beta together in order to calculate Rinf. Check for these first.
			float r25, beta;
			if (gb->Seen('T'))
			{
				r25 = gb->GetFValue();
				seen = true;
			}
			else
			{
				r25 = pp.GetThermistorR25();
			}
			if (gb->Seen('B'))
			{
				beta = gb->GetFValue();
				seen = true;
			}
			else
			{
				beta = pp.GetBeta();
			}

			if (seen)	// if see R25 or Beta or both
			{
				pp.SetThermistorR25AndBeta(r25, beta);					// recalculate Rinf
			}

			// Now do the other parameters
			if (gb->Seen('R'))
			{
				pp.thermistorSeriesR = gb->GetFValue();
				seen = true;
			}
			if (gb->Seen('L'))
			{
				pp.adcLowOffset = gb->GetFValue();
				seen = true;
			}
			if (gb->Seen('H'))
			{
				pp.adcHighOffset = gb->GetFValue();
				seen = true;
			}
			if (gb->Seen('X'))
			{
				int thermistor = gb->GetIValue();
				if (   (0 <= thermistor && thermistor < HEATERS)
					|| ((int)FirstThermocoupleChannel <= thermistor && thermistor < (int)(FirstThermocoupleChannel + MaxSpiTempSensors))
					|| ((int)FirstRtdChannel <= thermistor && thermistor < (int)(FirstRtdChannel + MaxSpiTempSensors))
				   )
				{
					platform->SetThermistorNumber(heater, thermistor);
				}
				else
				{
					platform->MessageF(GENERIC_MESSAGE, "Thermistor number %d is out of range\n", thermistor);
				}
				seen = true;
			}

			if (seen)
			{
				platform->SetPidParameters(heater, pp);
			}
			else
			{
				reply.printf("T:%.1f B:%.1f R:%.1f L:%.1f H:%.1f X:%d", r25, beta, pp.thermistorSeriesR,
						pp.adcLowOffset, pp.adcHighOffset, platform->GetThermistorNumber(heater));
			}
		}
		else
		{
			platform->MessageF(GENERIC_MESSAGE, "Heater number %d is out of range\n", heater);
		}
	}
}

void GCodes::SetToolHeaters(Tool *tool, float temperature)
{
	if (tool == NULL)
	{
		platform->Message(GENERIC_MESSAGE, "Setting temperature: no tool selected.\n");
		return;
	}

	float standby[HEATERS];
	float active[HEATERS];
	tool->GetVariables(standby, active);
	for (size_t h = 0; h < tool->HeaterCount(); h++)
	{
		active[h] = temperature;
	}
	tool->SetVariables(standby, active);
}

// Retract or un-retract filament, returning true if movement has been queued, false if this needs to be called again
bool GCodes::RetractFilament(bool retract)
{
	if (retractLength != 0.0 || retractHop != 0.0 || (retract && retractExtra != 0.0))
	{
		const Tool *tool = reprap.GetCurrentTool();
		if (tool != nullptr)
		{
			size_t nDrives = tool->DriveCount();
			if (nDrives != 0)
			{
				if (moveAvailable)
				{
					return false;
				}

				reprap.GetMove()->GetCurrentUserPosition(moveBuffer.coords, 0);
				for (size_t i = AXES; i < DRIVES; ++i)
				{
					moveBuffer.coords[i] = 0.0;
				}
				// Set the feed rate. If there is any Z hop then we need to pass the Z speed, else we pass the extrusion speed.
				moveBuffer.feedRate = (retractHop == 0.0)
										? retractSpeed * secondsToMinutes
										: retractSpeed * secondsToMinutes * retractHop/retractLength;
				moveBuffer.coords[Z_AXIS] += ((retract) ? retractHop : -retractHop);
				for (size_t i = 0; i < nDrives; ++i)
				{
					moveBuffer.coords[E0_AXIS + tool->Drive(i)] = (retract) ? -retractLength : retractLength + retractExtra;
				}

				moveBuffer.isFirmwareRetraction = true;
				moveBuffer.usePressureAdvance = false;
				moveBuffer.filePos = filePos;
				moveAvailable = true;
			}
		}
	}
	return true;
}

// If the code to act on is completed, this returns true,
// otherwise false.  It is called repeatedly for a given
// code until it returns true for that code.
bool GCodes::ActOnCode(GCodeBuffer *gb, StringRef& reply)
{
	// Discard empty buffers right away
	if (gb->IsEmpty())
	{
		return true;
	}

	gbCurrent = gb;

	// M-code parameters might contain letters T and G, e.g. in filenames.
	// dc42 assumes that G-and T-code parameters never contain the letter M.
	// Therefore we must check for an M-code first.
	if (gb->Seen('M'))
	{
		return HandleMcode(gb, reply);
	}
	// dc42 doesn't think a G-code parameter ever contains letter T, or a T-code ever contains letter G.
	// So it doesn't matter in which order we look for them.
	if (gb->Seen('G'))
	{
		return HandleGcode(gb, reply);
	}
	if (gb->Seen('T'))
	{
		return HandleTcode(gb, reply);
	}

	// An invalid or queued buffer gets discarded
	HandleReply(gb, false, "");
	return true;
}

bool GCodes::HandleGcode(GCodeBuffer* gb, StringRef& reply)
{
	bool result = true;
	bool error = false;

	int code = gb->GetIValue();
	if (simulationMode != 0 && code != 0 && code != 1 && code != 4 && code != 10 && code != 20 && code != 21 && code != 90 && code != 91 && code != 92)
	{
		return true;			// we only simulate some gcodes
	}

	switch (code)
	{
	case 0: // There are no rapid moves...
	case 1: // Ordinary move
		// Check for 'R' parameter here to go back to the coordinates at which the print was paused
		if (gb->Seen('R') && gb->GetIValue() > 0 && (IsPaused() || IsPausing() || IsResuming()))
		{
			if (moveAvailable)
			{
				return false;
			}
			for (size_t axis = 0; axis < AXES; ++axis)
			{
				float offset = gb->Seen(axisLetters[axis]) ? gb->GetFValue() * distanceScale : 0.0;
				moveBuffer.coords[axis] = pausedMoveBuffer[axis] + offset;
			}
			for (size_t drive = AXES; drive < DRIVES; ++drive)
			{
				moveBuffer.coords[drive] = 0.0;
			}
			if (gb->Seen(feedrateLetter))
			{
				moveBuffer.feedRate = (gb->Seen(feedrateLetter)) ? gb->GetFValue() : feedRate;
			}
			moveBuffer.filePos = noFilePosition;
			moveBuffer.usePressureAdvance = false;
			moveAvailable = true;
		}
		else
		{
			int res = SetUpMove(gb, reply);
			if (res == 2)
			{
				state = GCodeState::waitingForMoveToComplete;
			}
			result = (res != 0);
		}
		break;

	case 4: // Dwell
		result = DoDwell(gb);
		break;

	case 10: // Set/report offsets and temperatures, or retract
		if (gb->Seen('P'))
		{
			SetOrReportOffsets(reply, gb);
		}
		else
		{
			result = RetractFilament(true);
		}
		break;

	case 11: // Un-retract
		result = RetractFilament(false);
		break;

	case 20: // Inches (which century are we living in, here?)
		distanceScale = INCH_TO_MM;
		break;

	case 21: // mm
		distanceScale = 1.0;
		break;

	case 28: // Home
		result = DoHome(gb, reply, error);
		break;

	case 30: // Z probe/manually set at a position and set that as point P
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		if (reprap.GetMove()->IsDeltaMode() && !AllAxesAreHomed())
		{
			reply.copy("Must home a delta printer before bed probing");
			error = true;
		}
		else
		{
			result = SetSingleZProbeAtAPosition(gb, reply);
		}
		break;

	case 31: // Return the probe value, or set probe variables
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		result = SetPrintZProbe(gb, reply);
		break;

	case 32: // Probe Z at multiple positions and generate the bed transform
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		// Try to execute bed.g
		if (!DoFileMacro(BED_EQUATION_G, reprap.GetMove()->IsDeltaMode()))
		{
			// If we get here then we are not on a delta printer and there is no bed.g file
			if (axisIsHomed[X_AXIS] && axisIsHomed[Y_AXIS])
			{
				state = GCodeState::setBed1;		// no bed.g file, so use the coordinates specified by M557
			}
			else
			{
				// We can only do bed levelling if X and Y have already been homed
				reply.copy("Must home X and Y before bed probing");
				error = true;
			}
		}
		break;

	case 90: // Absolute coordinates
		// DC 2014-07-21 we no longer change the extruder settings in response to G90/G91 commands
		//drivesRelative = false;
		axesRelative = false;
		break;

	case 91: // Relative coordinates
		// DC 2014-07-21 we no longer change the extruder settings in response to G90/G91 commands
		//drivesRelative = true; // Non-axis movements (i.e. extruders)
		axesRelative = true;   // Axis movements (i.e. X, Y and Z)
		break;

	case 92: // Set position
		result = SetPositions(gb);
		break;

	default:
		error = true;
		reply.printf("invalid G Code: %s", gb->Buffer());
	}
	if (result && state == GCodeState::normal)
	{
		HandleReply(gb, error, reply.Pointer());
	}
	return result;
}

bool GCodes::HandleMcode(GCodeBuffer* gb, StringRef& reply)
{
	bool result = true;
	bool error = false;

	int code = gb->GetIValue();
	if (simulationMode != 0 && (code < 20 || code > 37) && code != 0 && code != 1 && code != 82 && code != 83 && code != 105 && code != 111 && code != 112 && code != 122 && code != 408 && code != 999)
	{
		return true;			// we don't yet simulate most M codes
	}

	switch (code)
	{
	case 0: // Stop
	case 1: // Sleep
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		// Reset everything
		CancelPrint();

		if (isPaused)
		{
			isPaused = false;
			reply.copy("Print cancelled");
			// If we are cancelling a paused print with M0 and cancel.g exists then run it and do nothing else
			if (code == 0)
			{
				if (DoFileMacro(CANCEL_G, false))
				{
					break;
				}
			}
		}

		state = (code == 0) ? GCodeState::stopping : GCodeState::sleeping;
		DoFileMacro((code == 0) ? STOP_G : SLEEP_G, false);
		break;

#if SUPPORT_ROLAND
	case 3: // Spin spindle
		if (reprap.GetRoland()->Active())
		{
			if (gb->Seen('S'))
			{
				result = reprap.GetRoland()->ProcessSpindle(gb->GetFValue());
			}
		}
		break;
#endif

	case 18: // Motors off
	case 84:
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		{
			bool seen = false;
			for (size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					axisIsHomed[axis] = false;
					platform->DisableDrive(axis);
					seen = true;
				}
			}

			if (gb->Seen(extrudeLetter))
			{
				long int eDrive[DRIVES - AXES];
				size_t eCount = DRIVES - AXES;
				gb->GetLongArray(eDrive, eCount);
				for (size_t i = 0; i < eCount; i++)
				{
					seen = true;
					if (eDrive[i] < 0 || (size_t)eDrive[i] >= DRIVES - AXES)
					{
						reply.printf("Invalid extruder number specified: %ld", eDrive[i]);
						error = true;
						break;
					}
					platform->DisableDrive(AXES + eDrive[i]);
				}
			}

			if (gb->Seen('S'))
			{
				seen = true;

				float idleTimeout = gb->GetFValue();
				if (idleTimeout < 0.0)
				{
					reply.copy("Idle timeouts cannot be negative!");
					error = true;
				}
				else
				{
					reprap.GetMove()->SetIdleTimeout(idleTimeout);
				}
			}

			if (!seen)
			{
				DisableDrives();
			}
		}
		break;

	case 20:		// List files on SD card
		{
			OutputBuffer *fileResponse;
			int sparam = (gb->Seen('S')) ? gb->GetIValue() : 0;
			const char* dir = (gb->Seen('P')) ? gb->GetString() : platform->GetGCodeDir();

			if (sparam == 2)
			{
				fileResponse = reprap.GetFilesResponse(dir, true);		// Send the file list in JSON format
			}
			else
			{
				if (!OutputBuffer::Allocate(fileResponse))
				{
					// Cannot allocate an output buffer, try again later
					return false;
				}

				// To mimic the behaviour of the official RepRapPro firmware:
				// If we are emulating RepRap then we print "GCode files:\n" at the start, otherwise we don't.
				// If we are emulating Marlin and the code came via the serial/USB interface, then we don't put quotes around the names and we separate them with newline;
				// otherwise we put quotes around them and separate them with comma.
				if (platform->Emulating() == me || platform->Emulating() == reprapFirmware)
				{
					fileResponse->copy("GCode files:\n");
				}

				bool encapsulateList = ((gb != serialGCode && gb != telnetGCode) || platform->Emulating() != marlin);
				FileInfo fileInfo;
				if (platform->GetMassStorage()->FindFirst(dir, fileInfo))
				{
					// iterate through all entries and append each file name
					do {
						if (encapsulateList)
						{
							fileResponse->catf("%c%s%c%c", FILE_LIST_BRACKET, fileInfo.fileName, FILE_LIST_BRACKET, FILE_LIST_SEPARATOR);
						}
						else
						{
							fileResponse->catf("%s\n", fileInfo.fileName);
						}
					} while (platform->GetMassStorage()->FindNext(fileInfo));

					if (encapsulateList)
					{
						// remove the last separator
						(*fileResponse)[fileResponse->Length() - 1] = 0;
					}
				}
				else
				{
					fileResponse->cat("NONE\n");
				}
			}

			HandleReply(gb, false, fileResponse);
			return true;
		}

	case 21: // Initialise SD card
		{
			size_t card = (gb->Seen('P')) ? gb->GetIValue() : 0;
			result = platform->GetMassStorage()->Mount(card, reply, true);
		}
		break;

	case 22: // Release SD card
		{
			size_t card = (gb->Seen('P')) ? gb->GetIValue() : 0;
			result = platform->GetMassStorage()->Unmount(card, reply);
		}
		break;

	case 23: // Set file to print
	case 32: // Select file and start SD print
		if (isPaused)
		{
			reply.copy("Cannot set file to print, because another print is still paused. Run M0 or M1 first.");
			break;
		}

		if (code == 32 && !AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		{
			const char* filename = gb->GetUnprecedentedString();
			if (filename != nullptr)
			{
				QueueFileToPrint(filename);
				if (fileToPrint.IsLive())
				{
					reprap.GetPrintMonitor()->StartingPrint(filename);
					if (platform->Emulating() == marlin && gb == serialGCode)
					{
						reply.copy("File opened\nFile selected");
					}
					else
					{
						// Command came from web interface or PanelDue, or not emulating Marlin, so send a nicer response
						reply.printf("File %s selected for printing", filename);
					}

					if (code == 32)
					{
						fileBeingPrinted.MoveFrom(fileToPrint);
						reprap.GetPrintMonitor()->StartedPrint();
					}
				}
				else
				{
					reply.printf("Failed to open file %s", filename);
				}
			}
		}
		break;

	case 24: // Print/resume-printing the selected file
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		if (isPaused)
		{
			state = GCodeState::resuming1;
			DoFileMacro(RESUME_G);
		}
		else if (!fileToPrint.IsLive())
		{
			reply.copy("Cannot print, because no file is selected!");
			error = true;
		}
		else
		{
			fileBeingPrinted.MoveFrom(fileToPrint);
			reprap.GetPrintMonitor()->StartedPrint();
		}
		break;

	case 226: // Gcode Initiated Pause
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			return false;
		// no break

	case 25: // Pause the print
		if (isPaused)
		{
			reply.copy("Printing is already paused!!");
			error = true;
		}
		else if (!reprap.GetPrintMonitor()->IsPrinting())
		{
			reply.copy("Cannot pause print, because no file is being printed!");
			error = true;
		}
		else if (doingFileMacro && gb != fileMacroGCode)
		{
			reply.copy("Cannot pause macro files, wait for it to complete first!");
			error = true;
		}
		else
		{
			DoPause(code == 25 && gb != fileGCode);
		}
		break;

	case 26: // Set SD position
		if (gb->Seen('S'))
		{
			long value = gb->GetLValue();
			if (value < 0)
			{
				reply.copy("SD positions can't be negative!");
				error = true;
			}
			else if (fileBeingPrinted.IsLive())
			{
				if (!fileBeingPrinted.Seek(value))
				{
					reply.copy("The specified SD position is invalid!");
					error = true;
				}
			}
			else if (fileToPrint.IsLive())
			{
				if (!fileToPrint.Seek(value))
				{
					reply.copy("The specified SD position is invalid!");
					error = true;
				}
			}
			else
			{
				reply.copy("Cannot set SD file position, because no print is in progress!");
				error = true;
			}
		}
		else
		{
			reply.copy("You must specify the SD position in bytes using the S parameter.");
			error = true;
		}
		break;

	case 27: // Report print status - Deprecated
		if (reprap.GetPrintMonitor()->IsPrinting())
		{
			// Pronterface keeps sending M27 commands if "Monitor status" is checked, and it specifically expects the following response syntax
			reply.printf("SD printing byte %lu/%lu", fileBeingPrinted.GetPosition(), fileBeingPrinted.Length());
		}
		else
		{
			reply.copy("Not SD printing.");
		}
		break;

	case 28: // Write to file
		{
			const char* str = gb->GetUnprecedentedString();
			if (str != nullptr)
			{
				bool ok = OpenFileToWrite(platform->GetGCodeDir(), str, gb);
				if (ok)
				{
					reply.printf("Writing to file: %s", str);
				}
				else
				{
					reply.printf("Can't open file %s for writing.", str);
					error = true;
				}
			}
		}
		break;

	case 29: // End of file being written; should be intercepted before getting here
		reply.copy("GCode end-of-file being interpreted.");
		break;

	case 30:	// Delete file
		{
			const char *filename = gb->GetUnprecedentedString();
			if (filename != nullptr)
			{
				DeleteFile(filename);
			}
		}
		break;

		// For case 32, see case 24

	case 36:	// Return file information
		{
			const char* filename = gb->GetUnprecedentedString(true);	// get filename, or nullptr if none provided
			OutputBuffer *fileInfoResponse;
			result = reprap.GetPrintMonitor()->GetFileInfoResponse(filename, fileInfoResponse);
			if (result)
			{
				fileInfoResponse->cat('\n');
				HandleReply(gb, false, fileInfoResponse);
				return true;
			}
		}
		break;

	case 37:	// Simulation mode on/off
		if (gb->Seen('S'))
		{
			if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			{
				return false;
			}

			bool wasSimulating = (simulationMode != 0);
			simulationMode = (uint8_t)gb->GetIValue();
			reprap.GetMove()->Simulate(simulationMode);

			if (simulationMode != 0)
			{
				simulationTime = 0.0;
				if (!wasSimulating)
				{
					// Starting a new simulation, so save the current position
					reprap.GetMove()->GetCurrentUserPosition(savedMoveBuffer, 0);
					savedFeedRate = feedRate;
				}
			}
			else if (wasSimulating)
			{
				// Ending a simulation, so restore the position
				SetPositions(savedMoveBuffer);
				for (size_t i = 0; i < DRIVES; ++i)
				{
					moveBuffer.coords[i] = savedMoveBuffer[i];
				}
				feedRate = savedFeedRate;
			}
		}
		else
		{
			reply.printf("Simulation mode: %s, move time: %.1f sec, other time: %.1f sec",
					(simulationMode != 0) ? "on" : "off", reprap.GetMove()->GetSimulationTime(), simulationTime);
		}
		break;

	case 38: // Report SHA1 of file
		if (fileBeingHashed == nullptr)
		{
			// See if we can open the file and start hashing
			const char* filename = gb->GetUnprecedentedString(true);
			if (StartHash(filename))
			{
				// Hashing is now in progress...
				result = false;
			}
			else
			{
				error = true;
				reply.printf("Cannot open file: %s", filename);
			}
		}
		else
		{
			// This can take some time. All the actual heavy lifting is in dedicated methods
			result = AdvanceHash(reply);
		}
		break;

	case 42:	// Turn an output pin on or off
		if (gb->Seen('P'))
		{
			int pin = gb->GetIValue();
			if (gb->Seen('S'))
			{
				int val = gb->GetIValue();
				bool success = platform->SetPin(pin, val);
				if (!success)
				{
					error = true;
					reply.printf("Setting pin %d to %d is not supported", pin, val);
				}
			}
		}
		break;

	case 80:	// ATX power on
		platform->SetAtxPower(true);
		break;

	case 81:	// ATX power off
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			return false;
		platform->SetAtxPower(false);
		break;

	case 82:	// Use absolute extruder positioning
		if (drivesRelative)		// don't reset the absolute extruder position if it was already absolute
		{
			for (size_t extruder = AXES; extruder < DRIVES; extruder++)
			{
				lastRawExtruderPosition[extruder - AXES] = 0.0;
			}
			drivesRelative = false;
		}
		break;

	case 83:	// Use relative extruder positioning
		if (!drivesRelative)	// don't reset the absolute extruder position if it was already relative
		{
			for (size_t extruder = AXES; extruder < DRIVES; extruder++)
			{
				lastRawExtruderPosition[extruder - AXES] = 0.0;
			}
			drivesRelative = true;
		}
		break;

		// For case 84, see case 18

	case 85: // Set inactive time
		break;

	case 92: // Set/report steps/mm for some axes
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		{
			// Save the current positions as we may need them later
			float positionNow[DRIVES];
			Move *move = reprap.GetMove();
			move->GetCurrentUserPosition(positionNow, 0);

			bool seen = false;
			for (size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					platform->SetDriveStepsPerUnit(axis, gb->GetFValue());
					seen = true;
				}
			}

			if (gb->Seen(extrudeLetter))
			{
				seen = true;
				float eVals[DRIVES - AXES];
				size_t eCount = DRIVES - AXES;
				gb->GetFloatArray(eVals, eCount, true);

				// The user may not have as many extruders as we allow for, so just set the ones for which a value is provided
				for (size_t e = 0; e < eCount; e++)
				{
					platform->SetDriveStepsPerUnit(AXES + e, eVals[e]);
				}
			}

			if (seen)
			{
				// On a delta, if we change the drive steps/mm then we need to recalculate the motor positions
				SetPositions(positionNow);
			}
			else
			{
				reply.printf("Steps/mm: X: %.3f, Y: %.3f, Z: %.3f, E: ", platform->DriveStepsPerUnit(X_AXIS),
						platform->DriveStepsPerUnit(Y_AXIS), platform->DriveStepsPerUnit(Z_AXIS));
				for (size_t drive = AXES; drive < DRIVES; drive++)
				{
					reply.catf("%.3f", platform->DriveStepsPerUnit(drive));
					if (drive < DRIVES - 1)
					{
						reply.cat(":");
					}
				}
			}
		}
		break;

	case 98: // Call Macro/Subprogram
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		if (gb->Seen('P'))
		{
			DoFileMacro(gb->GetString());
		}
		break;

	case 99: // Return from Macro/Subprogram
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		FileMacroCyclesReturn();
		break;

	case 104: // Deprecated.  This sets the active temperature of every heater of the active tool
		if (gb->Seen('S'))
		{
			float temperature = gb->GetFValue();
			Tool* tool;
			if (gb->Seen('T'))
			{
				int toolNumber = gb->GetIValue();
				toolNumber += gb->GetToolNumberAdjust();
				tool = reprap.GetTool(toolNumber);
			}
			else
			{
				tool = reprap.GetCurrentTool();
				// If no tool is selected, and there is only one tool, set the active temperature for that one
				if (tool == nullptr)
				{
					tool = reprap.GetOnlyTool();
				}
			}
			SetToolHeaters(tool, temperature);
		}
		break;

	case 105: // Get Extruder Temperature / Get Status Message
		{
			OutputBuffer *statusResponse;
			int param = (gb->Seen('S')) ? gb->GetIValue() : 0;
			int seq = (gb->Seen('R')) ? gb->GetIValue() : -1;
			switch (param)
			{
			// case 1 is reserved for future Pronterface versions, see http://reprap.org/wiki/G-code#M105:_Get_Extruder_Temperature
			// case 2 and 3 are used by old versions of PanelDue firmware. Later versions use M408 instead.

			/* NOTE: The following responses are subject to deprecation */
			case 2:
			case 3:
				statusResponse = reprap.GetLegacyStatusResponse(param, seq);	// send JSON-formatted status response
				if (statusResponse != nullptr)
				{
					statusResponse->cat('\n');
					HandleReply(gb, false, statusResponse);
					return true;
				}
				return false;

			default:
				reply.copy("T:");
				for (int8_t heater = 1; heater < HEATERS; heater++)
				{
					Heat::HeaterStatus hs = reprap.GetHeat()->GetStatus(heater);
					if (hs != Heat::HS_off && hs != Heat::HS_fault)
					{
						reply.catf("%.1f ", reprap.GetHeat()->GetTemperature(heater));
					}
				}
				reply.catf("B:%.1f", reprap.GetHeat()->GetTemperature(BED_HEATER));
				break;
			}
		}
		break;

	case 106: // Set/report fan values
		{
			bool seen = false;
			int fanNum = 0;			// Default to the first fan
			if (gb->Seen('P'))		// Choose fan number
			{
				fanNum = gb->GetIValue();
				if (fanNum < 0 || fanNum > (int)NUM_FANS)
				{
					reply.printf("Fan number %d is invalid, must be between 0 and %u", fanNum, NUM_FANS);
					break;
				}
			}

			if (gb->Seen('I'))		// Invert cooling
			{
				platform->SetCoolingInverted(fanNum, gb->GetIValue() > 0);
				seen = true;
			}

			if (gb->Seen('F'))		// Set PWM frequency
			{
				platform->SetFanPwmFrequency(fanNum, gb->GetFValue());
				seen = true;
			}

			if (gb->Seen('T'))		// Set thermostatic trigger temperature
			{
				seen = true;
				platform->SetTriggerTemperature(fanNum, gb->GetFValue());
			}

			if (gb->Seen('H'))		// Set thermostatically-controller heaters
			{
				seen = true;
				long heaters[HEATERS];
				size_t numH = HEATERS;
				gb->GetLongArray(heaters, numH);
				// Note that M106 H-1 disables thermostatic mode. The following code implements that automatically.
				uint16_t hh = 0;
				for (size_t h = 0; h < numH; ++h)
				{
					const int hnum = heaters[h];
					if (hnum >= 0 && hnum < HEATERS)
					{
						hh |= (1 << (unsigned int)hnum);
					}
				}
				if (hh != 0)
				{
					platform->SetFanValue(fanNum, 1.0);			// default the fan speed to full for safety
				}
				platform->SetHeatersMonitored(fanNum, hh);
			}

			if (gb->Seen('S'))		// Set new fan value - process this after processing 'H' or it may not be acted on
			{
				float f = gb->GetFValue();
				f = min<float>(f, 255.0);
				f = max<float>(f, 0.0);
				seen = true;
				platform->SetFanValue(fanNum, f);
			}
			else if (gb->Seen('R'))
			{
				seen = true;
				platform->SetFanValue(fanNum, pausedFanValues[fanNum]);
			}

			if (!seen)
			{
				reply.printf("Fan%i frequency: %dHz, speed: %d%%, inverted: %s",
								fanNum,
								(int)(platform->GetFanPwmFrequency(fanNum)),
								(int)(platform->GetFanValue(fanNum) * 100.0),
								(platform->GetCoolingInverted(fanNum)) ? "yes" : "no");
				uint16_t hh = platform->GetHeatersMonitored(fanNum);
				if (hh != 0)
				{
					reply.catf(", trigger: %dC, heaters:", (int)platform->GetTriggerTemperature(fanNum));
					for (unsigned int i = 0; i < HEATERS; ++i)
					{
						if ((hh & (1 << i)) != 0)
						{
							reply.catf(" %u", i);
						}
					}
				}
			}
		}
		break;

	case 107: // Fan off - deprecated
		platform->SetFanValue(0, 0.0);		//T3P3 as deprecated only applies to fan0
		break;

	case 109: // Deprecated
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
			return false;

		if (gb->Seen('S'))
		{
			float temperature = gb->GetFValue();
			Tool *tool;
			if (gb->Seen('T'))
			{
				int toolNumber = gb->GetIValue();
				toolNumber += gb->GetToolNumberAdjust();
				tool = reprap.GetTool(toolNumber);
			}
			else
			{
				tool = reprap.GetCurrentTool();
				// If no tool is selected, and there is only one tool, set the active temperature for that one
				if (tool == nullptr)
				{
					tool = reprap.GetOnlyTool();
				}
			}
			SetToolHeaters(tool, temperature);
			result = ToolHeatersAtSetTemperatures(tool);
		}
		break;

	case 110: // Set line numbers - line numbers are dealt with in the GCodeBuffer class
		break;

	case 111: // Debug level
		if (gb->Seen('S'))
		{
			bool dbv = (gb->GetIValue() != 0);
			if (gb->Seen('P'))
			{
				reprap.SetDebug(static_cast<Module>(gb->GetIValue()), dbv);
			}
			else
			{
				reprap.SetDebug(dbv);
			}
		}
		else
		{
			reprap.PrintDebug();
		}
		break;

	case 112: // Emergency stop - acted upon in Webserver, but also here in case it comes from USB etc.
		DoEmergencyStop();
		break;

	case 114:
		GetCurrentCoordinates(reply);
		break;

	case 115: // Print firmware version or set hardware type
		if (gb->Seen('P'))
		{
			platform->SetBoardType((BoardType)gb->GetIValue());
		}
		else
		{
			reply.printf("FIRMWARE_NAME: %s FIRMWARE_VERSION: %s ELECTRONICS: %s DATE: %s", NAME, VERSION, platform->GetElectronicsString(), DATE);
		}
		break;

	case 116: // Wait for everything, especially set temperatures
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		{
			bool seen = false;
			if (gb->Seen('P'))
			{
				// Wait for the heaters associated with the specified tool to be ready
				int toolNumber = gb->GetIValue();
				toolNumber += gb->GetToolNumberAdjust();
				if (!ToolHeatersAtSetTemperatures(reprap.GetTool(toolNumber)))
				{
					return false;
				}
				seen = true;
			}

			if (gb->Seen('H'))
			{
				// Wait for specified heaters to be ready
				long heaters[HEATERS];
				size_t heaterCount = HEATERS;
				gb->GetLongArray(heaters, heaterCount);
				for(size_t i=0; i<heaterCount; i++)
				{
					if (!reprap.GetHeat()->HeaterAtSetTemperature(heaters[i]))
					{
						return false;
					}
				}
				seen = true;
			}

			if (gb->Seen('C'))
			{
				// Wait for chamber heater to be ready
				const int8_t chamberHeater = reprap.GetHeat()->GetChamberHeater();
				if (chamberHeater != -1)
				{
					if (!reprap.GetHeat()->HeaterAtSetTemperature(chamberHeater))
					{
						return false;
					}
				}
				seen = true;
			}

			if (!seen)
			{
				// Wait for all heaters to be ready
				result = reprap.GetHeat()->AllHeatersAtSetTemperatures(true);
			}
		}
		break;

	case 117:	// Display message
		{
			const char *msg = gb->GetUnprecedentedString(true);
			reprap.SetMessage((msg == nullptr) ? "" : msg);
		}
		break;

	case 119:
		reply.copy("Endstops - ");
		for (size_t axis = 0; axis < AXES; axis++)
		{
			reply.catf("%c: %s, ", axisLetters[axis], TranslateEndStopResult(platform->Stopped(axis)));
		}
		reply.catf("Z probe: %s", TranslateEndStopResult(platform->GetZProbeResult()));
		break;

	case 120:
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		Push();
		break;

	case 121:
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		Pop();
		break;

	case 122:
		{
			int val = (gb->Seen('P')) ? gb->GetIValue() : 0;
			if (val == 0)
			{
				reprap.Diagnostics(gb->GetResponseMessageType());
			}
			else
			{
				platform->DiagnosticTest(val);
			}
		}
		break;

	case 126: // Valve open
		reply.copy("M126 - valves not yet implemented");
		break;

	case 127: // Valve closed
		reply.copy("M127 - valves not yet implemented");
		break;

	case 135: // Set PID sample interval
		if (gb->Seen('S'))
		{
			platform->SetHeatSampleTime(gb->GetFValue() * 0.001);  // Value is in milliseconds; we want seconds
		}
		else
		{
			reply.printf("Heat sample time is %.3f seconds", platform->GetHeatSampleTime());
		}
		break;

	case 140: // Set bed temperature
		{
			int8_t bedHeater;
			if (gb->Seen('H'))
			{
				bedHeater = gb->GetIValue();
				if (bedHeater < 0)
				{
					// Make sure we stay within reasonable boundaries...
					bedHeater = -1;

					// If we're disabling the hot bed, make sure the old heater is turned off
					reprap.GetHeat()->SwitchOff(reprap.GetHeat()->GetBedHeater());
				}
				else if (bedHeater >= HEATERS)
				{
					reply.copy("Invalid heater number!");
					error = true;
					break;
				}
				reprap.GetHeat()->SetBedHeater(bedHeater);

				if (bedHeater < 0)
				{
					// Stop here if the hot bed has been disabled
					break;
				}
			}
			else
			{
				bedHeater = reprap.GetHeat()->GetBedHeater();
				if (bedHeater < 0)
				{
					reply.copy("Hot bed is not present!");
					error = true;
					break;
				}
			}

			if(gb->Seen('S'))
			{
				float temperature = gb->GetFValue();
				if (temperature < NEARLY_ABS_ZERO)
				{
					reprap.GetHeat()->SwitchOff(bedHeater);
				}
				else
				{
					reprap.GetHeat()->SetActiveTemperature(bedHeater, temperature);
					reprap.GetHeat()->Activate(bedHeater);
				}
			}
			if(gb->Seen('R'))
			{
				reprap.GetHeat()->SetStandbyTemperature(bedHeater, gb->GetFValue());
			}
		}
		break;

	case 141: // Chamber temperature
		{
			bool seen = false;
			if (gb->Seen('H'))
			{
				seen = true;

				int heater = gb->GetIValue();
				if (heater < 0)
				{
					const int8_t currentHeater = reprap.GetHeat()->GetChamberHeater();
					if (currentHeater != -1)
					{
						reprap.GetHeat()->SwitchOff(currentHeater);
					}

					reprap.GetHeat()->SetChamberHeater(-1);
				}
				else if (heater < HEATERS)
				{
					reprap.GetHeat()->SetChamberHeater(heater);
				}
				else
				{
					reply.copy("Bad heater number specified!");
					error = true;
				}
			}

			if (gb->Seen('S'))
			{
				seen = true;

				const int8_t currentHeater = reprap.GetHeat()->GetChamberHeater();
				if (currentHeater != -1)
				{
					float temperature = gb->GetFValue();

					if (temperature < NEARLY_ABS_ZERO)
					{
						reprap.GetHeat()->SwitchOff(currentHeater);
					}
					else
					{
						reprap.GetHeat()->SetActiveTemperature(currentHeater, temperature);
						reprap.GetHeat()->Activate(currentHeater);
					}
				}
				else
				{
					reply.copy("No chamber heater has been set up yet!");
					error = true;
				}
			}

			if (!seen)
			{
				const int8_t currentHeater = reprap.GetHeat()->GetChamberHeater();
				if (currentHeater != -1)
				{
					reply.printf("Chamber heater %d is currently at %.1fC", currentHeater, reprap.GetHeat()->GetTemperature(currentHeater));
				}
				else
				{
					reply.copy("No chamber heater has been configured yet.");
				}
			}
		}
		break;

	case 143: // Set temperature limit
		if (gb->Seen('S'))
		{
			float limit = gb->GetFValue();
			if (limit > BAD_LOW_TEMPERATURE && limit < BAD_ERROR_TEMPERATURE)
			{
				platform->SetTemperatureLimit(limit);
			}
			else
			{
				reply.copy("Invalid temperature limit");
				error = true;
			}
		}
		else
		{
			reply.printf("Temperature limit is %.1fC", platform->GetTemperatureLimit());
		}
		break;

	case 144: // Set bed to standby
#if BED_HEATER >= 0
		reprap.GetHeat()->Standby(BED_HEATER);
#else
		reply.copy("Hot bed is not present!");
		error = true;
#endif
		break;

	case 190: // Set bed temperature and wait
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())	// tell Move not to wait for more moves
		{
			return false;
		}

		if (gb->Seen('S'))
		{
			if (BED_HEATER >= 0)
			{
				reprap.GetHeat()->SetActiveTemperature(BED_HEATER, gb->GetFValue());
				reprap.GetHeat()->Activate(BED_HEATER);
				result = reprap.GetHeat()->HeaterAtSetTemperature(BED_HEATER);
			}
		}
		break;

	case 201: // Set/print axis accelerations
	{
		bool seen = false;
		for (size_t axis = 0; axis < AXES; axis++)
		{
			if (gb->Seen(axisLetters[axis]))
			{
				platform->SetAcceleration(axis, gb->GetFValue() * distanceScale);
				seen = true;
			}
		}

		if (gb->Seen(extrudeLetter))
		{
			seen = true;
			float eVals[DRIVES - AXES];
			size_t eCount = DRIVES - AXES;
			gb->GetFloatArray(eVals, eCount, true);
			for (size_t e = 0; e < eCount; e++)
			{
				platform->SetAcceleration(AXES + e, eVals[e] * distanceScale);
			}
		}

		if (gb->Seen('P'))
		{
			// Set max average printing acceleration
			platform->SetMaxAverageAcceleration(gb->GetFValue() * distanceScale);
			seen = true;
		}

		if (!seen)
		{
			reply.printf("Accelerations: X: %.1f, Y: %.1f, Z: %.1f, E: ",
					platform->Acceleration(X_AXIS) / distanceScale, platform->Acceleration(Y_AXIS) / distanceScale,
					platform->Acceleration(Z_AXIS) / distanceScale);
			for (size_t drive = AXES; drive < DRIVES; drive++)
			{
				reply.catf("%.1f", platform->Acceleration(drive) / distanceScale);
				if (drive < DRIVES - 1)
				{
					reply.cat(":");
				}
			}
			reply.catf(", avg. printing: %.1f", platform->GetMaxAverageAcceleration());
		}
	}
		break;

	case 203: // Set/print maximum feedrates
		{
			bool seen = false;
			for (size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					platform->SetMaxFeedrate(axis, gb->GetFValue() * distanceScale * secondsToMinutes); // G Code feedrates are in mm/minute; we need mm/sec
					seen = true;
				}
			}

			if (gb->Seen(extrudeLetter))
			{
				seen = true;
				float eVals[DRIVES - AXES];
				size_t eCount = DRIVES - AXES;
				gb->GetFloatArray(eVals, eCount, true);
				for (size_t e = 0; e < eCount; e++)
				{
					platform->SetMaxFeedrate(AXES + e, eVals[e] * distanceScale * secondsToMinutes);
				}
			}

			if (!seen)
			{
				reply.printf("Maximum feedrates: X: %.1f, Y: %.1f, Z: %.1f, E: ",
						platform->MaxFeedrate(X_AXIS) / (distanceScale * secondsToMinutes),
						platform->MaxFeedrate(Y_AXIS) / (distanceScale * secondsToMinutes),
						platform->MaxFeedrate(Z_AXIS) / (distanceScale * secondsToMinutes));
				for (size_t drive = AXES; drive < DRIVES; drive++)
				{
					reply.catf("%.1f", platform->MaxFeedrate(drive) / (distanceScale * secondsToMinutes));
					if (drive < DRIVES - 1)
					{
						reply.cat(":");
					}
				}
			}
		}
		break;

	case 205: //M205 advanced settings:  minimum travel speed S=while printing T=travel only,  B=minimum segment time X= maximum xy jerk, Z=maximum Z jerk
		// This is superseded in this firmware by M codes for the separate types (e.g. M566).
		break;

	case 206:  // Offset axes - Deprecated
		result = OffsetAxes(gb);
		break;

	case 207: // Set firmware retraction details
		{
			bool seen = false;
			if (gb->Seen('S'))
			{
				retractLength = max<float>(gb->GetFValue(), 0.0);
				seen = true;
			}
			if (gb->Seen('R'))
			{
				retractExtra = max<float>(gb->GetFValue(), 0.0);
				seen = true;
			}
			if (gb->Seen('F'))
			{
				retractSpeed = max<float>(gb->GetFValue(), 60.0);
				seen = true;
			}
			if (gb->Seen('Z'))
			{
				retractHop = max<float>(gb->GetFValue(), 0.0);
				seen = true;
			}
			if (!seen)
			{
				reply.printf("Retraction settings: length %.2fmm, extra length %.2fmm, speed %dmm/min, Z hop %.2fmm",
						retractLength, retractExtra, (int)retractSpeed, retractHop);
			}
		}
		break;

	case 208: // Set/print maximum axis lengths. If there is an S parameter with value 1 then we set the min value, else we set the max value.
		{
			bool setMin = (gb->Seen('S') ? (gb->GetIValue() == 1) : false);
			bool seen = false;
			for (size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					float value = gb->GetFValue() * distanceScale;
					if (setMin)
					{
						platform->SetAxisMinimum(axis, value);
					}
					else
					{
						platform->SetAxisMaximum(axis, value);
					}
					seen = true;
				}
			}

			if (!seen)
			{
				reply.copy("Axis limits ");
				char sep = '-';
				for (size_t axis = 0; axis < AXES; axis++)
				{
					reply.catf("%c %c: %.1f min, %.1f max", sep, axisLetters[axis], platform->AxisMinimum(axis),
							platform->AxisMaximum(axis));
					sep = ',';
				}
			}
		}
		break;

	case 210: // Set/print homing feed rates
		// This is no longer used, but for backwards compatibility we don't report an error
		break;

	case 220:	// Set/report speed factor override percentage
		if (gb->Seen('S'))
		{
			float newSpeedFactor = (gb->GetFValue() / 100.0) * secondsToMinutes;	// include the conversion from mm/minute to mm/second
			if (newSpeedFactor > 0.0)
			{
				feedRate *= newSpeedFactor / speedFactor;
				if (moveAvailable && !moveBuffer.isFirmwareRetraction)
				{
					// The last move has not gone yet, so we can update it
					moveBuffer.feedRate *= newSpeedFactor / speedFactor;
				}
				speedFactor = newSpeedFactor;
			}
			else
			{
				reply.printf("Invalid speed factor specified.");
				error = true;
			}
		}
		else
		{
			reply.printf("Speed factor override: %.1f%%", speedFactor * minutesToSeconds * 100.0);
		}
		break;

	case 221:	// Set/report extrusion factor override percentage
		{
			int extruder = 0;
			if (gb->Seen('D'))	// D parameter (if present) selects the extruder number
			{
				extruder = gb->GetIValue();
			}

			if (gb->Seen('S'))	// S parameter sets the override percentage
			{
				float extrusionFactor = gb->GetFValue() / 100.0;
				if (extruder >= 0 && (size_t)extruder < DRIVES - AXES && extrusionFactor >= 0.0)
				{
					if (moveAvailable && !moveBuffer.isFirmwareRetraction)
					{
						moveBuffer.coords[extruder + AXES] *= extrusionFactor/extrusionFactors[extruder];	// last move not gone, so update it
					}
					extrusionFactors[extruder] = extrusionFactor;
				}
			}
			else
			{
				reply.printf("Extrusion factor override for extruder %d: %.1f%%", extruder,
						extrusionFactors[extruder] * 100.0);
			}
		}
		break;

		// For case 226, see case 25

	case 300:	// Beep
		{
			int ms = (gb->Seen('P')) ? gb->GetIValue() : 1000;			// time in milliseconds
			int freq = (gb->Seen('S')) ? gb->GetIValue() : 4600;		// 4600Hz produces the loudest sound on a PanelDue
			reprap.Beep(freq, ms);
		}
		break;

	case 301: // Set/report hot end PID values
		SetPidParameters(gb, 1, reply);
		break;

	case 302: // Allow, deny or report cold extrudes
		if (gb->Seen('P'))
		{
			if (gb->GetIValue() > 0)
			{
				reprap.GetHeat()->AllowColdExtrude();
			}
			else
			{
				reprap.GetHeat()->DenyColdExtrude();
			}
		}
		else
		{
			reply.printf("Cold extrusion is %s, use M302 P[1/0] to allow/deny it",
					reprap.GetHeat()->ColdExtrude() ? "allowed" : "denied");
		}
		break;

	case 303: // Run PID tuning
		if (gb->Seen('H'))
		{
			const size_t heater = gb->GetIValue();
			const float temperature = (gb->Seen('S')) ? gb->GetFValue() : 225.0;
			const float maxPwm = (gb->Seen('P')) ? gb->GetFValue() : 0.5;
			if (heater < HEATERS && maxPwm >= 0.1 && maxPwm <= 1.0 && temperature >= 55.0 && temperature <= platform->GetTemperatureLimit())
			{
				reprap.GetHeat()->StartAutoTune(heater, temperature, maxPwm, reply);
			}
			else
			{
				reply.printf("Bad parameter in M303 command");
			}
		}
		else
		{
			reprap.GetHeat()->GetAutoTuneStatus(reply);
		}
		break;

	case 304: // Set/report heated bed PID values
		if (BED_HEATER >= 0)
		{
			SetPidParameters(gb, BED_HEATER, reply);
		}
		break;

	case 305: // Set/report specific heater parameters
		SetHeaterParameters(gb, reply);
		break;

	case 307: // Set heater process model parameters
		if (gb->Seen('H'))
		{
			size_t heater = gb->GetIValue();
			if (heater < HEATERS)
			{
				const FopDt& model = reprap.GetHeat()->GetHeaterModel(heater);
				bool seen = false;
				float gain, tc, td, maxPwm;
				bool usePid;
				if (gb->Seen('A'))
				{
					gain = gb->GetFValue();
					seen = true;
				}
				else
				{
					gain = model.GetGain();
				}
				if (gb->Seen('C'))
				{
					tc = gb->GetFValue();
					seen = true;
				}
				else
				{
					tc = model.GetTimeConstant();
				}
				if (gb->Seen('D'))
				{
					td = gb->GetFValue();
					seen = true;
				}
				else
				{
					td = model.GetDeadTime();
				}
				if (gb->Seen('B'))
				{
					usePid = (gb->GetIValue() == 0);
					seen = true;
				}
				else
				{
					usePid = model.UsePid();
				}
				if (gb->Seen('S'))
				{
					maxPwm = gb->GetFValue();
					seen = true;
				}
				else
				{
					maxPwm = model.GetMaxPwm();
				}

				if (seen)
				{
					if (reprap.GetHeat()->SetHeaterModel(heater, gain, tc, td, maxPwm, usePid))
					{
						reprap.GetHeat()->UseModel(heater, true);
					}
					else
					{
						reply.copy("Error: bad model parameters");
					}
				}
				else
				{
					reply.printf("Heater %u model: gain %.1f, time constant %.1f, dead time %.1f, max PWM %.2f, in use: %s, mode: %s",
							heater, model.GetGain(), model.GetTimeConstant(), model.GetDeadTime(), model.GetMaxPwm(),
							(reprap.GetHeat()->IsModelUsed(heater)) ? "yes" : "no",
							(model.UsePid()) ? "PID" : "bang-bang");
					if (model.UsePid())
					{
						// When reporting the PID parameters, we scale them by 255 for compatibility with older firmware and other firmware
						const PidParams& spParams = model.GetPidParameters(false);
						const float scaledSpKp = 255.0 * spParams.kP;
						reply.catf("\nSetpoint change: P%.1f, I%.2f, D%.1f",
								scaledSpKp, scaledSpKp * spParams.recipTi, scaledSpKp * spParams.tD);
						const PidParams& ldParams = model.GetPidParameters(true);
						const float scaledLoadKp = 255.0 * ldParams.kP;
						reply.catf("\nLoad change: P%.1f, I%.2f, D%.1f",
								scaledLoadKp, scaledLoadKp * ldParams.recipTi, scaledLoadKp * ldParams.tD);
					}
				}
			}
		}
		break;

	case 350: // Set/report microstepping
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		{
			// interp is current an int not a bool, because we use special values of interp to set the chopper control register
			int interp = 0;
			if (gb->Seen('I'))
			{
				interp = gb->GetIValue();
			}

			bool seen = false;
			for (size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					seen = true;
					int microsteps = gb->GetIValue();
					if (ChangeMicrostepping(axis, microsteps, interp))
					{
						axisIsHomed[axis] = false;
					}
					else
					{
						platform->MessageF(GENERIC_MESSAGE, "Drive %c does not support %dx microstepping%s\n",
												axisLetters[axis], microsteps, (interp) ? " with interpolation" : "");
					}
				}
			}

			if (gb->Seen(extrudeLetter))
			{
				seen = true;
				long eVals[DRIVES - AXES];
				size_t eCount = DRIVES - AXES;
				gb->GetLongArray(eVals, eCount);
				for (size_t e = 0; e < eCount; e++)
				{
					if (!ChangeMicrostepping(AXES + e, (int)eVals[e], interp))
					{
						platform->MessageF(GENERIC_MESSAGE, "Drive E%u does not support %dx microstepping%s\n",
												e, (int)eVals[e], (interp) ? " with interpolation" : "");
					}
				}
			}

			if (!seen)
			{
				reply.copy("Microstepping - ");
				for (size_t axis = 0; axis < AXES; ++axis)
				{
					bool interp;
					int microsteps = platform->GetMicrostepping(axis, interp);
					reply.catf("%c:%d%s, ", axisLetters[axis], microsteps, (interp) ? "(on)" : "");
				}
				reply.cat("E");
				for (size_t drive = AXES; drive < DRIVES; drive++)
				{
					bool interp;
					int microsteps = platform->GetMicrostepping(drive, interp);
					reply.catf(":%d%s", microsteps, (interp) ? "(on)" : "");
				}
			}
		}
		break;

	case 400: // Wait for current moves to finish
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		break;

	case 404: // Filament width and nozzle diameter
		{
			bool seen = false;

			if (gb->Seen('N'))
			{
				platform->SetFilamentWidth(gb->GetFValue());
				seen = true;
			}
			if (gb->Seen('D'))
			{
				platform->SetNozzleDiameter(gb->GetFValue());
				seen = true;
			}

			if (!seen)
			{
				reply.printf("Filament width: %.2fmm, nozzle diameter: %.2fmm", platform->GetFilamentWidth(), platform->GetNozzleDiameter());
			}
		}
		break;

	case 408: // Get status in JSON format
		{
			int type = gb->Seen('S') ? gb->GetIValue() : 0;
			int seq = gb->Seen('R') ? gb->GetIValue() : -1;

			OutputBuffer *statusResponse = nullptr;
			switch (type)
			{
				case 0:
				case 1:
					statusResponse = reprap.GetLegacyStatusResponse(type + 2, seq);
					break;

				case 2:
				case 3:
				case 4:
					statusResponse = reprap.GetStatusResponse(type - 1, (gb == auxGCode) ? ResponseSource::AUX : ResponseSource::Generic);
					break;

				case 5:
					statusResponse = reprap.GetConfigResponse();
					break;
			}

			if (statusResponse != nullptr)
			{
				statusResponse->cat('\n');
				HandleReply(gb, false, statusResponse);
				return true;
			}
		}
		break;

	case 500: // Store parameters in EEPROM
		reprap.GetPlatform()->WriteNvData();
		break;

	case 501: // Load parameters from EEPROM
		reprap.GetPlatform()->ReadNvData();
		if (gb->Seen('S'))
		{
			reprap.GetPlatform()->SetAutoSave(gb->GetIValue() > 0);
		}
		break;

	case 502: // Revert to default "factory settings"
		reprap.GetPlatform()->ResetNvData();
		break;

	case 503: // List variable settings
		{
			// Need a valid output buffer to continue...
			OutputBuffer *configResponse;
			if (!OutputBuffer::Allocate(configResponse))
			{
				// No buffer available, try again later
				return false;
			}

			// Read the entire file
			FileStore *f = platform->GetFileStore(platform->GetSysDir(), platform->GetConfigFile(), false);
			if (f == nullptr)
			{
				error = true;
				reply.copy("Configuration file not found!");
			}
			else
			{
				char fileBuffer[FILE_BUFFER_SIZE];
				size_t bytesRead, bytesLeftForWriting = OutputBuffer::GetBytesLeft(configResponse);
				while ((bytesRead = f->Read(fileBuffer, FILE_BUFFER_SIZE)) > 0 && bytesLeftForWriting > 0)
				{
					// Don't write more data than we can process
					if (bytesRead < bytesLeftForWriting)
					{
						bytesLeftForWriting -= bytesRead;
					}
					else
					{
						bytesRead = bytesLeftForWriting;
						bytesLeftForWriting = 0;
					}

					// Write it
					configResponse->cat(fileBuffer, bytesRead);
				}
				f->Close();

				HandleReply(gb, false, configResponse);
				return true;
			}
		}
		break;

	case 540: // Set/report MAC address
		if (gb->Seen('P'))
		{
			SetMACAddress(gb);
		}
		else
		{
			const byte* mac = platform->MACAddress();
			reply.printf("MAC: %x:%x:%x:%x:%x:%x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
		}
		break;

	case 550: // Set/report machine name
		if (gb->Seen('P'))
		{
			reprap.SetName(gb->GetString());
		}
		else
		{
			reply.printf("RepRap name: %s", reprap.GetName());
		}
		break;

	case 551: // Set password (no option to report it)
		if (gb->Seen('P'))
		{
			reprap.SetPassword(gb->GetString());
		}
		break;

	case 552: // Enable/Disable network and/or Set/Get IP address
		{
			bool seen = false;
			if (gb->Seen('P'))
			{
				seen = true;
				SetEthernetAddress(gb, code);
			}

			if (gb->Seen('R'))
			{
				reprap.GetNetwork()->SetHttpPort(gb->GetIValue());
				seen = true;
			}

			// Process this one last in case the IP address is changed and the network enabled in the same command
			if (gb->Seen('S')) // Has the user turned the network on or off?
			{
				seen = true;
				if (gb->GetIValue() != 0)
				{
					reprap.GetNetwork()->Enable();
				}
				else
				{
					reprap.GetNetwork()->Disable();
				}
			}

			if (!seen)
			{
				const byte *config_ip = platform->IPAddress();
				const byte *actual_ip = reprap.GetNetwork()->IPAddress();
				reply.printf("Network is %s, configured IP address: %d.%d.%d.%d, actual IP address: %d.%d.%d.%d, HTTP port: %d",
						reprap.GetNetwork()->IsEnabled() ? "enabled" : "disabled",
						config_ip[0], config_ip[1], config_ip[2], config_ip[3], actual_ip[0], actual_ip[1], actual_ip[2], actual_ip[3],
						reprap.GetNetwork()->GetHttpPort());
			}
		}
		break;

	case 553: // Set/Get netmask
		if (gb->Seen('P'))
		{
			SetEthernetAddress(gb, code);
		}
		else
		{
			const byte *nm = platform->NetMask();
			reply.printf("Net mask: %d.%d.%d.%d ", nm[0], nm[1], nm[2], nm[3]);
		}
		break;

	case 554: // Set/Get gateway
		if (gb->Seen('P'))
		{
			SetEthernetAddress(gb, code);
		}
		else
		{
			const byte *gw = platform->GateWay();
			reply.printf("Gateway: %d.%d.%d.%d ", gw[0], gw[1], gw[2], gw[3]);
		}
		break;

	case 555: // Set/report firmware type to emulate
		if (gb->Seen('P'))
		{
			platform->SetEmulating((Compatibility) gb->GetIValue());
		}
		else
		{
			reply.copy("Emulating ");
			switch (platform->Emulating())
			{
			case me:
			case reprapFirmware:
				reply.cat("RepRap Firmware (i.e. in native mode)");
				break;

			case marlin:
				reply.cat("Marlin");
				break;

			case teacup:
				reply.cat("Teacup");
				break;

			case sprinter:
				reply.cat("Sprinter");
				break;

			case repetier:
				reply.cat("Repetier");
				break;

			default:
				reply.catf("Unknown: (%d)", platform->Emulating());
			}
		}
		break;

	case 556: // Axis compensation
		if (gb->Seen('S'))
		{
			float value = gb->GetFValue();
			for (size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					reprap.GetMove()->SetAxisCompensation(axis, gb->GetFValue() / value);
				}
			}
		}
		else
		{
			reply.printf("Axis compensations - XY: %.5f, YZ: %.5f, ZX: %.5f",
					reprap.GetMove()->AxisCompensation(X_AXIS), reprap.GetMove()->AxisCompensation(Y_AXIS),
					reprap.GetMove()->AxisCompensation(Z_AXIS));
		}
		break;

	case 557: // Set/report Z probe point coordinates
		if (gb->Seen('P'))
		{
			int point = gb->GetIValue();
			if (point < 0 || (unsigned int)point >= MAX_PROBE_POINTS)
			{
				reprap.GetPlatform()->Message(GENERIC_MESSAGE, "Z probe point index out of range.\n");
			}
			else
			{
				bool seen = false;
				if (gb->Seen(axisLetters[X_AXIS]))
				{
					reprap.GetMove()->SetXBedProbePoint(point, gb->GetFValue());
					seen = true;
				}
				if (gb->Seen(axisLetters[Y_AXIS]))
				{
					reprap.GetMove()->SetYBedProbePoint(point, gb->GetFValue());
					seen = true;
				}

				if (!seen)
				{
					reply.printf("Probe point %d - [%.1f, %.1f]", point, reprap.GetMove()->XBedProbePoint(point),
							reprap.GetMove()->YBedProbePoint(point));
				}
			}
		}
		break;

	case 558: // Set or report Z probe type and for which axes it is used
		{
			bool seenAxes = false, seenType = false, seenParam = false;
			bool zProbeAxes[AXES];
			platform->GetZProbeAxes(zProbeAxes);
			for (size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					zProbeAxes[axis] = (gb->GetIValue() > 0);
					seenAxes = true;
				}
			}
			if (seenAxes)
			{
				platform->SetZProbeAxes(zProbeAxes);
			}

			// We must get and set the Z probe type first before setting the dive height, because different probe types may have different dive heights
			if (gb->Seen('P'))		// probe type
			{
				platform->SetZProbeType(gb->GetIValue());
				seenType = true;
			}

			ZProbeParameters params = platform->GetZProbeParameters();
			if (gb->Seen('H'))		// dive height
			{
				params.diveHeight = gb->GetFValue();
				seenParam = true;
			}

			if (gb->Seen('F'))		// feed rate i.e. probing speed
			{
				params.probeSpeed = gb->GetFValue() * secondsToMinutes;
				seenParam = true;
			}

			if (gb->Seen('T'))		// travel speed to probe point
			{
				params.travelSpeed = gb->GetFValue() * secondsToMinutes;
				seenParam = true;
			}

			if (gb->Seen('S'))		// extra parameter for experimentation
			{
				params.param1 = gb->GetFValue();
				seenParam = true;
			}

			if (gb->Seen('R'))		// extra parameter for experimentation
			{
				params.param2 = gb->GetFValue();
				seenParam = true;
			}

			if (seenParam)
			{
				platform->SetZProbeParameters(params);
			}

			if (!(seenAxes || seenType || seenParam))
			{
				reply.printf("Z Probe type %d, dive height %.1fmm, probe speed %dmm/min, travel speed %dmm/min",
								platform->GetZProbeType(), platform->GetZProbeDiveHeight(),
								(int)(platform->GetZProbeParameters().probeSpeed * minutesToSeconds), (int)(platform->GetZProbeTravelSpeed() * minutesToSeconds));
				if (platform->GetZProbeType() == ZProbeTypeDelta)
				{
					ZProbeParameters params = platform->GetZProbeParameters();
					reply.catf(", parameters %.2f %.2f", params.param1, params.param2);
				}
				reply.cat(", used for these axes:");
				for (size_t axis = 0; axis < AXES; axis++)
				{
					if (zProbeAxes[axis])
					{
						reply.catf(" %c", axisLetters[axis]);
					}
				}
			}
		}
		break;

	case 559: // Upload config.g or another gcode file to put in the sys directory
	{
		const char* str = (gb->Seen('P') ? gb->GetString() : platform->GetConfigFile());
		bool ok = OpenFileToWrite(platform->GetSysDir(), str, gb);
		if (ok)
		{
			reply.printf("Writing to file: %s", str);
		}
		else
		{
			reply.printf("Can't open file %s for writing.", str);
			error = true;
		}
	}
		break;

	case 560: // Upload reprap.htm or another web interface file
	{
		const char* str = (gb->Seen('P') ? gb->GetString() : INDEX_PAGE_FILE);
		bool ok = OpenFileToWrite(platform->GetWebDir(), str, gb);
		if (ok)
		{
			reply.printf("Writing to file: %s", str);
		}
		else
		{
			reply.printf("Can't open file %s for writing.", str);
			error = true;
		}
	}
		break;

	case 561:
		reprap.GetMove()->SetIdentityTransform();
		break;

	case 562: // Reset temperature fault - use with great caution
		if (gb->Seen('P'))
		{
			int heater = gb->GetIValue();
			if (heater >= 0 && heater < HEATERS)
			{
				reprap.ClearTemperatureFault(heater);
			}
			else
			{
				reply.copy("Invalid heater number.\n");
				error = true;
			}
		}
		break;

	case 563: // Define tool
		ManageTool(gb, reply);
		break;

	case 564: // Think outside the box?
		if (gb->Seen('S'))
		{
			limitAxes = (gb->GetIValue() != 0);
		}
		else
		{
			reply.printf("Movement outside the bed is %spermitted", (limitAxes) ? "not " : "");
		}
		break;

	case 566: // Set/print maximum jerk speeds
	{
		bool seen = false;
		for (size_t axis = 0; axis < AXES; axis++)
		{
			if (gb->Seen(axisLetters[axis]))
			{
				platform->SetInstantDv(axis, gb->GetFValue() * distanceScale * secondsToMinutes); // G Code feedrates are in mm/minute; we need mm/sec
				seen = true;
			}
		}

		if (gb->Seen(extrudeLetter))
		{
			seen = true;
			float eVals[DRIVES - AXES];
			size_t eCount = DRIVES - AXES;
			gb->GetFloatArray(eVals, eCount, true);
			for (size_t e = 0; e < eCount; e++)
			{
				platform->SetInstantDv(AXES + e, eVals[e] * distanceScale * secondsToMinutes);
			}
		}
		else if (!seen)
		{
			reply.printf("Maximum jerk rates: X: %.1f, Y: %.1f, Z: %.1f, E:",
					platform->ConfiguredInstantDv(X_AXIS) / (distanceScale * secondsToMinutes),
					platform->ConfiguredInstantDv(Y_AXIS) / (distanceScale * secondsToMinutes),
					platform->ConfiguredInstantDv(Z_AXIS) / (distanceScale * secondsToMinutes));
			char sep = ' ';
			for (size_t drive = AXES; drive < DRIVES; drive++)
			{
				reply.catf("%c%.1f", sep, platform->ConfiguredInstantDv(drive) / (distanceScale * secondsToMinutes));
				sep = ':';
			}
		}
	}
		break;

	case 567: // Set/report tool mix ratios
		if (gb->Seen('P'))
		{
			int8_t tNumber = gb->GetIValue();
			Tool* tool = reprap.GetTool(tNumber);
			if (tool != NULL)
			{
				if (gb->Seen(extrudeLetter))
				{
					float eVals[DRIVES - AXES];
					size_t eCount = tool->DriveCount();
					gb->GetFloatArray(eVals, eCount, false);
					if (eCount != tool->DriveCount())
					{
						reply.printf("Setting mix ratios - wrong number of E drives: %s", gb->Buffer());
					}
					else
					{
						tool->DefineMix(eVals);
					}
				}
				else
				{
					reply.printf("Tool %d mix ratios:", tNumber);
					char sep = ' ';
					for (size_t drive = 0; drive < tool->DriveCount(); drive++)
					{
						reply.catf("%c%.3f", sep, tool->GetMix()[drive]);
						sep = ':';
					}
				}
			}
		}
		break;

	case 568: // Turn on/off automatic tool mixing
		if (gb->Seen('P'))
		{
			Tool* tool = reprap.GetTool(gb->GetIValue());
			if (tool != NULL)
			{
				if (gb->Seen('S'))
				{
					tool->SetMixing(gb->GetIValue() != 0);
				}
				else
				{
					reply.printf("Tool %d mixing is %s", tool->Number(), (tool->GetMixing()) ? "enabled" : "disabled");
				}
			}
		}
		break;

	case 569: // Set/report axis direction
		if (gb->Seen('P'))
		{
			size_t drive = gb->GetIValue();
			if (drive < DRIVES)
			{
				bool seen = false;
				if (gb->Seen('S'))
				{
					platform->SetDirectionValue(drive, gb->GetIValue() != 0);
					seen = true;
				}
				if (gb->Seen('R'))
				{
					platform->SetEnableValue(drive, gb->GetIValue() != 0);
					seen = true;
				}
				if (gb->Seen('T'))
				{
					platform->SetDriverStepTiming(drive, gb->GetFValue());
					seen = true;
				}
				bool badParameter = false;
				for (size_t axis = 0; axis < AXES; ++axis)
				{
					if (gb->Seen(axisLetters[axis]))
					{
						badParameter = true;
					}
				}
				if (gb->Seen(extrudeLetter))
				{
					badParameter = true;
				}
				if (badParameter)
				{
					platform->Message(GENERIC_MESSAGE, "Error: M569 no longer accepts XYZE parameters; use M584 instead\n");
				}
				else if (!seen)
				{
					reply.printf("A %d sends drive %u forwards, a %d enables it, and the minimum pulse width is %.1f microseconds",
								(int)platform->GetDirectionValue(drive), drive,
								(int)platform->GetEnableValue(drive),
								platform->GetDriverStepTiming(drive));
				}
			}
		}
		break;

	case 570: // Set/report heater timeout
		reply.copy("M570 is no longer required or supported");
		break;

	case 571: // Set output on extrude
		if (gb->Seen('S'))
		{
			platform->SetExtrusionAncilliaryPWM(gb->GetFValue());
		}
		else
		{
			reply.printf("Extrusion ancillary PWM: %.3f.", platform->GetExtrusionAncilliaryPWM());
		}
		break;

	case 572: // Set/report elastic compensation
		if (gb->Seen('D'))
		{
			// New usage: specify the extruder drive using the D parameter
			size_t extruder = gb->GetIValue();
			if (gb->Seen('S'))
			{
				platform->SetElasticComp(extruder, gb->GetFValue());
			}
			else
			{
				reply.printf("Pressure advance for extruder %u is %.3f seconds", extruder, platform->GetElasticComp(extruder));
			}
		}
		break;

	case 573: // Report heater average PWM
		if (gb->Seen('P'))
		{
			int heater = gb->GetIValue();
			if (heater >= 0 && heater < HEATERS)
			{
				reply.printf("Average heater %d PWM: %.3f", heater, reprap.GetHeat()->GetAveragePWM(heater));
			}
		}
		break;

	case 574: // Set endstop configuration
		{
			bool seen = false;
			bool logicLevel = (gb->Seen('S')) ? (gb->GetIValue() != 0) : true;
			for (size_t axis = 0; axis <= AXES; ++axis)
			{
				const char letter = (axis == AXES) ? extrudeLetter : axisLetters[axis];
				if (gb->Seen(letter))
				{
					int ival = gb->GetIValue();
					if (ival >= 0 && ival <= 3)
					{
						platform->SetEndStopConfiguration(axis, (EndStopType) ival, logicLevel);
						seen = true;
					}
				}
			}
			if (!seen)
			{
				reply.copy("Endstop configuration:");
				EndStopType config;
				bool logic;
				for (size_t axis = 0; axis < AXES; ++axis)
				{
					platform->GetEndStopConfiguration(axis, config, logic);
					reply.catf(" %c %s (active %s),", axisLetters[axis],
							(config == EndStopType::highEndStop) ? "high end" : (config == EndStopType::lowEndStop) ? "low end" : "none",
							(config == EndStopType::noEndStop) ? "" : (logic) ? "high" : "low");
				}
				platform->GetEndStopConfiguration(E0_AXIS, config, logic);
				reply.catf(" E (active %s)", (logic) ? "high" : "low");
			}
		}
		break;

	case 575: // Set communications parameters
		if (gb->Seen('P'))
		{
			size_t chan = gb->GetIValue();
			if (chan < NUM_SERIAL_CHANNELS)
			{
				bool seen = false;
				if (gb->Seen('B'))
				{
					platform->SetBaudRate(chan, gb->GetIValue());
					seen = true;
				}
				if (gb->Seen('S'))
				{
					uint32_t val = gb->GetIValue();
					platform->SetCommsProperties(chan, val);
					switch (chan)
					{
					case 0:
						serialGCode->SetCommsProperties(val);
						break;
					case 1:
						auxGCode->SetCommsProperties(val);
						break;
					default:
						break;
					}
					seen = true;
				}
				if (!seen)
				{
					uint32_t cp = platform->GetCommsProperties(chan);
					reply.printf("Channel %d: baud rate %d, %s checksum", chan, platform->GetBaudRate(chan),
							(cp & 1) ? "requires" : "does not require");
				}
			}
		}
		break;

	case 577: // Wait until endstop is triggered
		if (gb->Seen('S'))
		{
			// Determine trigger type
			EndStopHit triggerCondition;
			switch (gb->GetIValue())
			{
				case 1:
					triggerCondition = EndStopHit::lowHit;
					break;
				case 2:
					triggerCondition = EndStopHit::highHit;
					break;
				case 3:
					triggerCondition = EndStopHit::lowNear;
					break;
				default:
					triggerCondition = EndStopHit::noStop;
					break;
			}

			// Axis endstops
			for(size_t axis=0; axis<AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					if (platform->Stopped(axis) != triggerCondition)
					{
						result = false;
						break;
					}
				}
			}

			// Extruder drives
			size_t eDriveCount = DRIVES - AXES;
			long eDrives[DRIVES - AXES];
			if (gb->Seen(extrudeLetter))
			{
				gb->GetLongArray(eDrives, eDriveCount);
				for(size_t extruder=0; extruder<DRIVES - AXES; extruder++)
				{
					const size_t eDrive = eDrives[extruder] + AXES;
					if (eDrive < AXES || eDrive >= DRIVES)
					{
						reply.copy("Invalid extruder drive specified!");
						error = result = true;
						break;
					}

					if (platform->Stopped(eDrive) != triggerCondition)
					{
						result = false;
						break;
					}
				}
			}
		}
		break;

#if SUPPORT_INKJET
	case 578: // Fire Inkjet bits
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}

		if (gb->Seen('S')) // Need to handle the 'P' parameter too; see http://reprap.org/wiki/G-code#M578:_Fire_inkjet_bits
		{
			platform->Inkjet(gb->GetIValue());
		}
		break;
#endif

	case 579: // Scale Cartesian axes (mostly for Delta configurations)
		{
			bool seen = false;
			for(size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					axisScaleFactors[axis] = gb->GetFValue();
					seen = true;
				}
			}

			if (!seen)
			{
				char sep = ':';
				reply.copy("Axis scale factors");
				for(size_t axis = 0; axis < AXES; axis++)
				{
					reply.catf("%c %c: %.3f", sep, axisLetters[axis], axisScaleFactors[axis]);
					sep = ',';
				}
			}
		}
		break;

#if SUPPORT_ROLAND
	case 580: // (De)Select Roland mill
		if (gb->Seen('R'))
		{
			if (gb->GetIValue())
			{
				reprap.GetRoland()->Activate();
				if (gb->Seen('P'))
				{
					result = reprap.GetRoland()->RawWrite(gb->GetString());
				}
			}
			else
			{
				result = reprap.GetRoland()->Deactivate();
			}
		}
		else
		{
			reply.printf("Roland is %s.", reprap.GetRoland()->Active() ? "active" : "inactive");
		}
		break;
#endif

	case 581: // Configure external trigger
	case 582: // Check external trigger
		if (gb->Seen('T'))
		{
			unsigned int triggerNumber = gb->GetIValue();
			if (triggerNumber < MaxTriggers)
			{
				if (code == 582)
				{
					uint32_t states = platform->GetAllEndstopStates();
					if ((triggers[triggerNumber].rising & states) != 0 || (triggers[triggerNumber].falling & ~states) != 0)
					{
						triggersPending |= (1 << triggerNumber);
					}
				}
				else
				{
					bool seen = false;
					if (gb->Seen('C'))
					{
						seen = true;
						triggers[triggerNumber].condition = gb->GetIValue();
					}
					else if (triggers[triggerNumber].IsUnused())
					{
						triggers[triggerNumber].condition = 0;		// this is a new trigger, so set no condition
					}
					if (gb->Seen('S'))
					{
						seen = true;
						int sval = gb->GetIValue();
						TriggerMask triggerMask = 0;
						for (size_t axis = 0; axis < AXES; ++axis)
						{
							if (gb->Seen(axisLetters[axis]))
							{
								triggerMask |= (1u << axis);
							}
						}
						if (gb->Seen(extrudeLetter))
						{
							long eStops[DRIVES - AXES];
							size_t numEntries = DRIVES - AXES;
							gb->GetLongArray(eStops, numEntries);
							for (size_t i = 0; i < numEntries; ++i)
							{
								if (eStops[i] >= 0 && (unsigned long)eStops[i] < DRIVES - AXES)
								{
									triggerMask |= (1u << (eStops[i] + AXES));
								}
							}
						}
						switch(sval)
						{
						case -1:
							if (triggerMask == 0)
							{
								triggers[triggerNumber].rising = triggers[triggerNumber].falling = 0;
							}
							else
							{
								triggers[triggerNumber].rising &= (~triggerMask);
								triggers[triggerNumber].falling &= (~triggerMask);
							}
							break;

						case 0:
							triggers[triggerNumber].falling |= triggerMask;
							break;

						case 1:
							triggers[triggerNumber].rising |= triggerMask;
							break;

						default:
							platform->Message(GENERIC_MESSAGE, "Bad S parameter in M581 command\n");
						}
					}
					if (!seen)
					{
						reply.printf("Trigger %u fires on a rising edge on ", triggerNumber);
						ListTriggers(reply, triggers[triggerNumber].rising);
						reply.cat(" or a falling edge on ");
						ListTriggers(reply, triggers[triggerNumber].falling);
						reply.cat(" endstop inputs");
						if (triggers[triggerNumber].condition == 1)
						{
							reply.cat(" when printing from SD card");
						}
					}
				}
			}
			else
			{
				platform->Message(GENERIC_MESSAGE, "Trigger number out of range\n");
			}
		}
		break;

	case 584: // Set axis/extruder to stepper driver(s) mapping
		{
			bool seen = false, badDrive = false;
			for (size_t drive = 0; drive < AXES; ++drive)
			{
				if (gb->Seen(axisLetters[drive]))
				{
					seen = true;
					size_t numValues = MaxDriversPerAxis;
					long drivers[MaxDriversPerAxis];
					gb->GetLongArray(drivers, numValues);

					// Check all the driver numbers are in range
					bool badAxis = false;
					AxisDriversConfig config;
					config.numDrivers = numValues;
					for (size_t i = 0; i < numValues; ++i)
					{
						if ((unsigned long)drivers[i] >= DRIVES)
						{
							badAxis = true;
						}
						else
						{
							config.driverNumbers[i] = (uint8_t)drivers[i];
						}
					}
					if (badAxis)
					{
						badDrive = true;
					}
					else
					{
						platform->SetAxisDriversConfig(drive, config);
					}
				}
			}

			if (gb->Seen(extrudeLetter))
			{
				seen = true;
				size_t numValues = DRIVES - AXES;
				long drivers[DRIVES - AXES];
				gb->GetLongArray(drivers, numValues);
				for (size_t i = 0; i < numValues; ++i)
				{
					if ((unsigned long)drivers[i] >= DRIVES)
					{
						badDrive = true;
					}
					else
					{
						platform->SetExtruderDriver(i, (uint8_t)drivers[i]);
					}
				}
			}

			if (badDrive)
			{
				platform->Message(GENERIC_MESSAGE, "Error: invalid drive number in M584 command\n");
			}
			else if (!seen)
			{
				reply.copy("Driver assignments:");
				for (size_t drive = 0; drive < AXES; ++ drive)
				{
					reply.cat(' ');
					const AxisDriversConfig& axisConfig = platform->GetAxisDriversConfig(drive);
					char c = axisLetters[drive];
					for (size_t i = 0; i < axisConfig.numDrivers; ++i)
					{
						reply.catf("%c%u", c, axisConfig.driverNumbers[i]);
						c = ':';
					}
				}
				reply.cat(' ');
				char c = extrudeLetter;
				for (size_t extruder = 0; extruder < DRIVES - AXES; ++extruder)
				{
					reply.catf("%c%u", c, platform->GetExtruderDriver(extruder));
					c = ':';
				}
			}
		}
		break;

	case 665: // Set delta configuration
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		{
			float positionNow[DRIVES];
			Move *move = reprap.GetMove();
			move->GetCurrentUserPosition(positionNow, 0);					// get the current position, we may need it later
			DeltaParameters& params = move->AccessDeltaParams();
			bool wasInDeltaMode = params.IsDeltaMode();						// remember whether we were in delta mode
			bool seen = false;

			if (gb->Seen('L'))
			{
				params.SetDiagonal(gb->GetFValue() * distanceScale);
				seen = true;
			}
			if (gb->Seen('R'))
			{
				params.SetRadius(gb->GetFValue() * distanceScale);
				seen = true;
			}
			if (gb->Seen('B'))
			{
				params.SetPrintRadius(gb->GetFValue() * distanceScale);
				seen = true;
			}
			if (gb->Seen('X'))
			{
				// X tower position correction
				params.SetXCorrection(gb->GetFValue());
				seen = true;
			}
			if (gb->Seen('Y'))
			{
				// Y tower position correction
				params.SetYCorrection(gb->GetFValue());
				seen = true;
			}
			if (gb->Seen('Z'))
			{
				// Y tower position correction
				params.SetZCorrection(gb->GetFValue());
				seen = true;
			}

			// The homed height must be done last, because it gets recalculated when some of the other factors are changed
			if (gb->Seen('H'))
			{
				params.SetHomedHeight(gb->GetFValue() * distanceScale);
				seen = true;
			}

			if (seen)
			{
				move->SetCoreXYMode(0);		// CoreXYMode needs to be zero when executing special moves on a delta

				// If we have changed between Cartesian and Delta mode, we need to reset the motor coordinates to agree with the XYZ coordinates.
				// This normally happens only when we process the M665 command in config.g. Also flag that the machine is not homed.
				if (params.IsDeltaMode() != wasInDeltaMode)
				{
					SetPositions(positionNow);
				}
				SetAllAxesNotHomed();
			}
			else
			{
				if (params.IsDeltaMode())
				{
					reply.printf("Diagonal %.2f, delta radius %.2f, homed height %.2f, bed radius %.1f"
								 ", X %.2f" DEGREE_SYMBOL ", Y %.2f" DEGREE_SYMBOL ", Z %.2f" DEGREE_SYMBOL,
								 	 params.GetDiagonal() / distanceScale, params.GetRadius() / distanceScale,
								 	 params.GetHomedHeight() / distanceScale, params.GetPrintRadius() / distanceScale,
								 	 params.GetXCorrection(), params.GetYCorrection(), params.GetZCorrection());
				}
				else
				{
					reply.printf("Printer is not in delta mode");
				}
			}
		}
		break;

	case 666: // Set delta endstop adjustments
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		{
			DeltaParameters& params = reprap.GetMove()->AccessDeltaParams();
			bool seen = false;
			if (gb->Seen('X'))
			{
				params.SetEndstopAdjustment(X_AXIS, gb->GetFValue());
				seen = true;
			}
			if (gb->Seen('Y'))
			{
				params.SetEndstopAdjustment(Y_AXIS, gb->GetFValue());
				seen = true;
			}
			if (gb->Seen('Z'))
			{
				params.SetEndstopAdjustment(Z_AXIS, gb->GetFValue());
				seen = true;
			}

			if (seen)
			{
				SetAllAxesNotHomed();
			}
			else
			{
				reply.printf("Endstop adjustments X%.2f Y%.2f Z%.2f",
						params.GetEndstopAdjustment(X_AXIS), params.GetEndstopAdjustment(Y_AXIS), params.GetEndstopAdjustment(Z_AXIS));
			}
		}
		break;

	case 667: // Set CoreXY mode
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		{
			Move* move = reprap.GetMove();
			bool seen = false;
			float positionNow[DRIVES];
			move->GetCurrentUserPosition(positionNow, 0);					// get the current position, we may need it later
			if (gb->Seen('S'))
			{
				move->SetCoreXYMode(gb->GetIValue());
				seen = true;
			}
			for (size_t axis = 0; axis < AXES; ++axis)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					move->setCoreAxisFactor(axis, gb->GetFValue());
					seen = true;
				}
			}

			if (seen)
			{
				SetPositions(positionNow);
				SetAllAxesNotHomed();
			}
			else
			{
				reply.printf("Printer mode is %s with axis factors", move->GetGeometryString());
				for (size_t axis = 0; axis < AXES; ++axis)
				{
					reply.catf(" %c:%f", axisLetters[axis], move->GetCoreAxisFactor(axis));
				}
			}
		}
		break;

	case 906: // Set/report Motor currents
	case 913: // Set/report motor current percent
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		{
			bool seen = false;
			for (size_t axis = 0; axis < AXES; axis++)
			{
				if (gb->Seen(axisLetters[axis]))
				{
					platform->SetMotorCurrent(axis, gb->GetFValue(), code == 913);
					seen = true;
				}
			}

			if (gb->Seen(extrudeLetter))
			{
				float eVals[DRIVES - AXES];
				size_t eCount = DRIVES - AXES;
				gb->GetFloatArray(eVals, eCount, true);
				// 2014-09-29 DC42: we no longer insist that the user supplies values for all possible extruder drives
				for (size_t e = 0; e < eCount; e++)
				{
					platform->SetMotorCurrent(AXES + e, eVals[e], code == 913);
				}
				seen = true;
			}

			if (code == 906 && gb->Seen('I'))
			{
				float idleFactor = gb->GetFValue();
				if (idleFactor >= 0 && idleFactor <= 100.0)
				{
					platform->SetIdleCurrentFactor(idleFactor/100.0);
					seen = true;
				}
			}

			if (!seen)
			{
				reply.copy((code == 913) ? "Motor current % of normal - " : "Motor current (mA) - ");
				for (size_t axis = 0; axis < AXES; ++axis)
				{
					reply.catf("%c:%d, ", axisLetters[axis], (int)platform->GetMotorCurrent(axis, code == 913));
				}
				reply.cat("E");
				for (size_t drive = AXES; drive < DRIVES; drive++)
				{
					reply.catf(":%d", (int)platform->GetMotorCurrent(drive, code == 913));
				}
				if (code == 906)
				{
					reply.catf(", idle factor %d%%", (int)(platform->GetIdleCurrentFactor() * 100.0));
				}
			}
		}
		break;

	case 911: // Set power monitor threshold voltages
		reply.printf("M911 not implemented yet");
		break;

	case 912: // Set electronics temperature monitor adjustment
		// Currently we ignore the P parameter (i.e. temperature measurement channel)
		if (gb->Seen('S'))
		{
			platform->SetMcuTemperatureAdjust(gb->GetFValue());
		}
		else
		{
			reply.printf("MCU temperature calibration adjustment is %.1f" DEGREE_SYMBOL "C", platform->GetMcuTemperatureAdjust());
		}
		break;

	// For case 913, see 906

	case 997: // Perform firmware update
		if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
		{
			return false;
		}
		reprap.GetHeat()->SwitchOffAll();					// turn all heaters off because the main loop may get suspended
		DisableDrives();									// all motors off

		if (firmwareUpdateModuleMap == 0)					// have we worked out which modules to update?
		{
			// Find out which modules we have been asked to update
			if (gb->Seen('S'))
			{
				long modulesToUpdate[3];
				size_t numUpdateModules = ARRAY_SIZE(modulesToUpdate);
				gb->GetLongArray(modulesToUpdate, numUpdateModules);
				for (size_t i = 0; i < numUpdateModules; ++i)
				{
					long t = modulesToUpdate[i];
					if (t < 0 || (unsigned long)t >= NumFirmwareUpdateModules)
					{
						platform->MessageF(GENERIC_MESSAGE, "Invalid module number '%ld'\n", t);
						firmwareUpdateModuleMap = 0;
						break;
					}
					firmwareUpdateModuleMap |= (1 << (unsigned int)t);
				}
			}
			else
			{
				firmwareUpdateModuleMap = (1 << 0);			// no modules specified, so update module 0 to match old behaviour
			}

			if (firmwareUpdateModuleMap == 0)
			{
				break;										// nothing to update
			}

			// Check prerequisites of all modules to be updated, if any are not met then don't update any of them
#ifdef DUET_NG
			if (!FirmwareUpdater::CheckFirmwareUpdatePrerequisites(firmwareUpdateModuleMap))
			{
				firmwareUpdateModuleMap = 0;
				break;
			}
#endif
			if ((firmwareUpdateModuleMap & 1) != 0 && !platform->CheckFirmwareUpdatePrerequisites())
			{
				firmwareUpdateModuleMap = 0;
				break;
			}
		}

		// If we get here then we have the module map, and all prerequisites are satisfied
		isFlashing = true;					// this tells the web interface and PanelDue that we are about to flash firmware
		if (!DoDwellTime(1.0))				// wait a second so all HTTP clients and PanelDue are notified
		{
			return false;
		}

		state = GCodeState::flashing1;
		break;

	case 998:
		// The input handling code replaces the gcode by this when it detects a checksum error.
		// Since we have no way of asking for the line to be re-sent, just report an error.
		// If the line number is zero, then it probably came from PanelDue and was caused by input buffer overflow
		// while we were busy doing a macro, so just ignore it.
		if (gb->Seen('P'))
		{
			int val = gb->GetIValue();
			if (val != 0)
			{
				reply.printf("Checksum error on line %d", val);
			}
		}
		break;

	case 999:
		result = DoDwellTime(0.5);			// wait half a second to allow the response to be sent back to the web server, otherwise it may retry
		if (result)
		{
			reprap.EmergencyStop();			// this disables heaters and drives - Duet WiFi seems to need drives disabled here
			uint16_t reason = (gb->Seen('P') && StringStartsWith(gb->GetString(), "ERASE"))
											? (uint16_t)SoftwareResetReason::erase
											: (uint16_t)SoftwareResetReason::user;
			platform->SoftwareReset(reason);			// doesn't return
		}
		break;

	default:
		error = true;
		reply.printf("invalid M Code: %s", gb->Buffer());
	}

	// Note that we send a reply to M105 requests even if the status is not 'normal', because we reply to these requests even when we are in other states
	if (result && (state == GCodeState::normal || GCodeBuffer::IsPollCode(code)))
	{
		HandleReply(gb, error, reply.Pointer());
	}
	return result;
}

bool GCodes::HandleTcode(GCodeBuffer* gb, StringRef& reply)
{
	if (!AllMovesAreFinishedAndMoveBufferIsLoaded())
	{
		return false;
	}

	newToolNumber = gb->GetIValue();
	newToolNumber += gb->GetToolNumberAdjust();
	if (simulationMode != 0)						// we don't yet simulate any T codes
	{
		HandleReply(gb, false, "");
	}
	else
	{
		// If old and new are the same still follow the sequence - the user may want the macros run.
		Tool *oldTool = reprap.GetCurrentTool();
		state = GCodeState::toolChange1;
		if (oldTool != nullptr && AllAxesAreHomed())
		{
			scratchString.printf("tfree%d.g", oldTool->Number());
			DoFileMacro(scratchString.Pointer(), false);
		}
	}
	return true;
}

// Return the amount of filament extruded
float GCodes::GetRawExtruderPosition(size_t extruder) const
{
	return (extruder < (DRIVES - AXES)) ? lastRawExtruderPosition[extruder] : 0.0;
}

float GCodes::GetRawExtruderTotalByDrive(size_t extruder) const
{
	return (extruder < (DRIVES - AXES)) ? rawExtruderTotalByDrive[extruder] : 0.0;
}

// Cancel the current SD card print
void GCodes::CancelPrint()
{
	moveAvailable = false;

	fileGCode->Init();

	if (fileBeingPrinted.IsLive())
	{
		fileBeingPrinted.Close();
	}

	reprap.GetPrintMonitor()->StoppedPrint();
}

// Return true if all the heaters for the specified tool are at their set temperatures
bool GCodes::ToolHeatersAtSetTemperatures(const Tool *tool) const
{
	if (tool != NULL)
	{
		for (size_t i = 0; i < tool->HeaterCount(); ++i)
		{
			if (!reprap.GetHeat()->HeaterAtSetTemperature(tool->Heater(i)))
			{
				return false;
			}
		}
	}
	return true;
}

// Set the current position
void GCodes::SetPositions(float positionNow[DRIVES])
{
	// Transform the position so that e.g. if the user does G92 Z0,
	// the position we report (which gets inverse-transformed) really is Z=0 afterwards
	reprap.GetMove()->Transform(positionNow);
	reprap.GetMove()->SetLiveCoordinates(positionNow);
	reprap.GetMove()->SetPositions(positionNow);
}

bool GCodes::IsPaused() const
{
	return isPaused && !IsPausing() && !IsResuming();
}

bool GCodes::IsPausing() const
{
	const GCodeState topState = (stackPointer == 0) ? state : stack[0].state;
	return topState == GCodeState::pausing1 || topState == GCodeState::pausing2;
}

bool GCodes::IsResuming() const
{
	const GCodeState topState = (stackPointer == 0) ? state : stack[0].state;
	return topState == GCodeState::resuming1 || topState == GCodeState::resuming2 || topState == GCodeState::resuming3;
}

bool GCodes::IsRunning() const
{
	return !IsPaused() && !IsPausing() && !IsResuming();
}

const char *GCodes::TranslateEndStopResult(EndStopHit es)
{
	switch (es)
	{
	case EndStopHit::lowHit:
		return "at min stop";

	case EndStopHit::highHit:
		return "at max stop";

	case EndStopHit::lowNear:
		return "near min stop";

	case EndStopHit::noStop:
	default:
		return "not stopped";
	}
}

// Append a list of trigger endstops to a message
void GCodes::ListTriggers(StringRef reply, TriggerMask mask)
{
	if (mask == 0)
	{
		reply.cat("(none)");
	}
	else
	{
		bool printed = false;
		for (unsigned int i = 0; i < DRIVES; ++i)
		{
			if ((mask & (1 << i)) != 0)
			{
				if (printed)
				{
					reply.cat(' ');
				}
				if (i < AXES)
				{
					reply.cat(axisLetters[i]);
				}
				else
				{
					reply.catf("E%d", i - AXES);
				}
				printed = true;
			}
		}
	}
}

// M38 (SHA1 hash of a file) implementation:
bool GCodes::StartHash(const char* filename)
{
	// Get a FileStore object
	fileBeingHashed = platform->GetFileStore(FS_PREFIX, filename, false);
	if (fileBeingHashed == nullptr)
	{
		return false;
	}

	// Start hashing
	SHA1Reset(&hash);
	return true;
}

bool GCodes::AdvanceHash(StringRef &reply)
{
	// Read and process some more data from the file
	uint32_t buf32[(FILE_BUFFER_SIZE + 3) / 4];
	char *buffer = reinterpret_cast<char *>(buf32);

	int bytesRead = fileBeingHashed->Read(buffer, FILE_BUFFER_SIZE);
	if (bytesRead != -1)
	{
		SHA1Input(&hash, reinterpret_cast<const uint8_t *>(buffer), bytesRead);

		if (bytesRead != FILE_BUFFER_SIZE)
		{
			// Calculate and report the final result
			SHA1Result(&hash);
			for(size_t i = 0; i < 5; i++)
			{
				reply.catf("%x", hash.Message_Digest[i]);
			}

			// Clean up again
			fileBeingHashed->Close();
			fileBeingHashed = nullptr;
			return true;
		}
		return false;
	}

	// Something went wrong, we cannot read any more from the file
	fileBeingHashed->Close();
	fileBeingHashed = nullptr;
	return true;
}

// End
