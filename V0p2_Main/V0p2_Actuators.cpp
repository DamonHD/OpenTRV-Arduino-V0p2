/*
The OpenTRV project licenses this file to you
under the Apache Licence, Version 2.0 (the "Licence");
you may not use this file except in compliance
with the Licence. You may obtain a copy of the Licence at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing,
software distributed under the Licence is distributed on an
"AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
KIND, either express or implied. See the Licence for the
specific language governing permissions and limitations
under the Licence.

Author(s) / Copyright (s): Damon Hart-Davis 2014--2015
*/

/*
 V0p2 boards physical actuator support.
 */
#include <stdint.h>
#include <limits.h>
#include <util/atomic.h>

#include <Wire.h> // Arduino I2C library.

#include "V0p2_Main.h"
#include "V0p2_Board_IO_Config.h" // I/O pin allocation: include ahead of I/O module headers.
#include "V0p2_Actuators.h" // I/O code access.

#include "Serial_IO.h"
#include "Power_Management.h"




#ifdef DIRECT_MOTOR_DRIVE_V1

// IF DEFINED: turn on lights to match motor drive for debug purposes.
#define MOTOR_DEBUG_LEDS


// Approx minimum time to let H-bridge settle/stabilise (ms).
static const uint8_t minMotorHBridgeSettleMS = 16;
// Min sub-cycle ticks for H-bridge to settle.
static const uint8_t minMotorHBridgeSettleTicks = max(1, minMotorHBridgeSettleMS / SUBCYCLE_TICK_MS_RD);

// Approx minimum runtime to get motor up to speed and not give false high-current readings (ms).
static const uint8_t minMotorRunupMS = 32;
// Min sub-cycle ticks to run up.
static const uint8_t minMotorRunupTicks = max(1, minMotorRunupMS / SUBCYCLE_TICK_MS_RD);

// Approx minimum runtime to get motor to reverse and up to speed and not give false high-current readings (ms).
static const uint8_t minMotorReverseMS = 128;
// Min sub-cycle ticks to run up.
static const uint8_t minMotorReverseTicks = max(1, minMotorReverseMS / SUBCYCLE_TICK_MS_RD);

// Call to actually run/stop low-level motor.
// May take as much as 200ms eg to change direction.
// Stopping (removing power) should typically be very fast, << 100ms.
//   * dir    direction to run motor (or off/stop)
//   * callback  callback handler
//   * start  if true then this routine starts the motor from cold,
//            else this runs the motor for a short continuation period;
//            at least one continuation should be performed before testing
//            for high current loads at end stops
// TODO: implement start
void ValveMotorDirectV1HardwareDriver::motorRun(const motor_drive dir, HardwareMotorDriverInterfaceCallbackHandler &callback, const bool start)
  {
  // *** MUST NEVER HAVE L AND R LOW AT THE SAME TIME else board may be destroyed at worst. ***
  // Operates as quickly as reasonably possible, eg to move to stall detection quickly...
  // TODO: consider making atomic to block some interrupt-related accidents...
  // TODO: note that the mapping between L/R and open/close not yet defined.
  // DHD20150205: 1st cut REV7 all-in-in-valve, seen looking down from valve into base, cw => close (ML=HIGH), ccw = open (MR=HIGH).
  switch(dir)
    {
    case motorDriveClosing:
      {
      // Pull one side high immediately *FIRST* for safety.
      // Stops motor if other side is not already low.
      // (Has no effect if motor is already running in the correct direction.)
      fastDigitalWrite(MOTOR_DRIVE_ML, HIGH);
      pinMode(MOTOR_DRIVE_ML, OUTPUT); // Ensure that the HIGH side is an output (can be done after, as else will be safe weak pull-up).
#ifdef MOTOR_DEBUG_LEDS
LED_UI2_OFF();
#endif
      OTV0P2BASE::nap(WDTO_120MS); // Let H-bridge respond and settle, and motor slow down.
      pinMode(MOTOR_DRIVE_MR, OUTPUT); // Ensure that the LOW side is an output.
      fastDigitalWrite(MOTOR_DRIVE_MR, LOW); // Pull LOW last.
#ifdef MOTOR_DEBUG_LEDS
LED_HEATCALL_ON();
#endif
      OTV0P2BASE::nap(WDTO_60MS); // Let H-bridge respond and settle and let motor run up.
      break; // Fall through to common case.
      }

    case motorDriveOpening:
      {
      // Pull one side high immediately *FIRST* for safety.
      // Stops motor if other side is not already low.
      // (Has no effect if motor is already running in the correct direction.)
      fastDigitalWrite(MOTOR_DRIVE_MR, HIGH); 
      pinMode(MOTOR_DRIVE_MR, OUTPUT); // Ensure that the HIGH side is an output (can be done after, as else will be safe weak pull-up).
#ifdef MOTOR_DEBUG_LEDS
LED_HEATCALL_OFF();
#endif
      OTV0P2BASE::nap(WDTO_120MS); // Let H-bridge respond and settle, and motor slow down.
      pinMode(MOTOR_DRIVE_ML, OUTPUT); // Ensure that the LOW side is an output.
      fastDigitalWrite(MOTOR_DRIVE_ML, LOW); // Pull LOW last.   
#ifdef MOTOR_DEBUG_LEDS
LED_UI2_ON();
#endif
      OTV0P2BASE::nap(WDTO_60MS); // Let H-bridge respond and settle and let motor run up.
      break; // Fall through to common case.
      }

    case motorOff: default: // Explicit off, and default for safety.
      {
      // Everything off...
      fastDigitalWrite(MOTOR_DRIVE_MR, HIGH); // Belt and braces force pin logical output state high.
      pinMode(MOTOR_DRIVE_MR, INPUT_PULLUP); // Switch to weak pull-up; slow but possibly marginally safer.
#ifdef MOTOR_DEBUG_LEDS
LED_HEATCALL_OFF();
#endif
      OTV0P2BASE::nap(WDTO_15MS); // Let H-bridge respond and settle.
      fastDigitalWrite(MOTOR_DRIVE_ML, HIGH); // Belt and braces force pin logical output state high.
      pinMode(MOTOR_DRIVE_ML, INPUT_PULLUP); // Switch to weak pull-up; slow but possibly marginally safer.
#ifdef MOTOR_DEBUG_LEDS
LED_UI2_OFF();
#endif
      OTV0P2BASE::nap(WDTO_15MS); // Let H-bridge respond and settle.
      return; // Return, not fall through.
      }
    }
  }


// DHD20151015: possible basis of calibration code
// Run motor ~1s in the current direction; reverse at end of travel.
//  DEBUG_SERIAL_PRINT_FLASHSTRING("Dir: ");
//  DEBUG_SERIAL_PRINT(HardwareMotorDriverInterface::motorDriveClosing == mdir ? "closing" : "opening");
//  DEBUG_SERIAL_PRINTLN();
//  bool currentHigh = false;
//  V1D.motorRun(mdir);
//  static uint16_t count;
//  uint8_t sctStart = getSubCycleTime();
//  uint8_t sctMinRunTime = sctStart + 4; // Min run time 32ms to avoid false readings.
//  uint8_t sct;
//  while(((sct = getSubCycleTime()) <= ((3*GSCT_MAX)/4)) && !(currentHigh = V1D.isCurrentHigh(mdir)))
//      { 
////      if(HardwareMotorDriverInterface::motorDriveClosing == mdir)
////        {
////        // BE VERY CAREFUL HERE: a wrong move could destroy the H-bridge.
////        fastDigitalWrite(MOTOR_DRIVE_MR, HIGH); // Blip high to remove power.
////        while(getSubCycleTime() == sct) { } // Off for ~8ms.
////        fastDigitalWrite(MOTOR_DRIVE_MR, LOW); // Pull LOW to re-enable power.
////        }
//      // Wait until end of tick or minimum period.
//      if(sct < sctMinRunTime) { while(getSubCycleTime() <= sctMinRunTime) { } }
//      else { while(getSubCycleTime() == sct) { } }
//      }
//  uint8_t sctEnd = getSubCycleTime();
//  // Stop motor until next loop (also ensures power off).
//  V1D.motorRun(HardwareMotorDriverInterface::motorOff);
//  // Detect if end-stop is reached or motor current otherwise very high and reverse.
//  count += (sctEnd - sctStart);
//  if(currentHigh)
//    {
//    DEBUG_SERIAL_PRINT_FLASHSTRING("Current high (reversing) at tick count ");
//    DEBUG_SERIAL_PRINT(count);
//    DEBUG_SERIAL_PRINTLN();
//    // DHD20151013:
//    //Typical run is 1400 to 1500 ticks
//    //(128 ticks = 1s, so 1 tick ~7.8ms)
//    //with closing taking longer
//    //(against the valve spring)
//    //than opening.
//    //
//    //Min run period to avoid false end-stop reports
//    //is ~30ms or ~4 ticks.
//    //
//    //Implies a nominal precision of ~4/1400 or << 1%,
//    //but an accuracy of ~1500/1400 as poor as ~10%.
//    count = 0;
//    // Reverse.
//    mdir = (HardwareMotorDriverInterface::motorDriveClosing == mdir) ?
//      HardwareMotorDriverInterface::motorDriveOpening : HardwareMotorDriverInterface::motorDriveClosing;
//    }


// IF DEFINED: MI output swing asymmetric or is not enough to use fast comparator.
#define MI_NEEDS_ADC

// Maximum current reading allowed when closing the valve (against the spring).
static const uint16_t maxCurrentReadingClosing = 600;
// Maximum current reading allowed when opening the valve (retracting the pin, no resisting force).
static const uint16_t maxCurrentReadingOpening = 400;

// Detect if end-stop is reached or motor current otherwise very high.] indicating stall.
bool ValveMotorDirectV1HardwareDriver::isCurrentHigh(HardwareMotorDriverInterface::motor_drive mdir) const
  {
  // Check for high motor current indicating hitting an end-stop.
#if !defined(MI_NEEDS_ADC)
  const bool currentSense = analogueVsBandgapRead(MOTOR_DRIVE_MI_AIN, true);
#else
  // Measure motor current against (fixed) internal reference.
  const uint16_t mi = analogueNoiseReducedRead(MOTOR_DRIVE_MI_AIN, INTERNAL);
  const uint16_t miHigh = (HardwareMotorDriverInterface::motorDriveClosing == mdir) ?
      maxCurrentReadingClosing : maxCurrentReadingOpening;
  const bool currentSense = (mi > miHigh) &&
    // Recheck the value read in case spiky.
    (analogueNoiseReducedRead(MOTOR_DRIVE_MI_AIN, INTERNAL) > miHigh) && (analogueNoiseReducedRead(MOTOR_DRIVE_MI_AIN, INTERNAL) > miHigh);
//  if(mi > ((2*miHigh)/4)) { DEBUG_SERIAL_PRINT(mi); DEBUG_SERIAL_PRINTLN(); }
#endif
  return(currentSense);
  }


//// Enable/disable end-stop detection and shaft-encoder.
//// Disabling should usually force the motor off,
//// with a small pause for any residual movement to complete.
//void ValveMotorDirectV1HardwareDriver::enableFeedback(const bool enable, HardwareMotorDriverInterfaceCallbackHandler &callback)
//  {
//  // Check for high motor current indicating hitting an end-stop.
//  const bool highI = isCurrentHigh();
////  if(highI) { LED_UI2_ON(); } else { LED_UI2_OFF(); }
//  if(highI) { callback.signalHittingEndStop(); } 
//  }

// Singleton implementation/instance.
ValveMotorDirectV1 ValveDirect;
#endif




// Minimally wiggles the motor to give tactile feedback and/or show to be working.
// May take a significant fraction of a second.
// Finishes with the motor turned off.
void CurrentSenseValveMotorDirect::wiggle()
  {
  hw->motorRun(HardwareMotorDriverInterface::motorOff, *this);
  hw->motorRun(HardwareMotorDriverInterface::motorDriveOpening, *this, true);
  hw->motorRun(HardwareMotorDriverInterface::motorDriveClosing, *this, true);
  hw->motorRun(HardwareMotorDriverInterface::motorOff, *this);
  }


// Poll.
// Regular poll every 1s or 2s,
// though tolerates missed polls eg because of other time-critical activity.
// May block for hundreds of milliseconds.
void CurrentSenseValveMotorDirect::poll()
  {
  // Run the state machine based on the major state.
  switch(state)
    {
    // Power-up: move to 'pin withdrawing' state and possibly start a timer.
    case init:
      {
DEBUG_SERIAL_PRINTLN_FLASHSTRING("  init");
      wiggle(); // Tactile feedback and ensure that the motor is left stopped.
      state = valvePinWithdrawing;
      // TODO: record time withdrawl starts (to allow time out).
      break;
      }

    // Fully withdrawing pin (nominally opening valve) to make valve head easy to fit.
    case valvePinWithdrawing:
      {
DEBUG_SERIAL_PRINTLN_FLASHSTRING("  valvePinWithdrawing");
      endStopDetected = false; // Clear the end-stop detection flag ready.
      bool currentHigh = false;
      hw->motorRun(HardwareMotorDriverInterface::motorDriveOpening, *this, true);
      uint8_t sctStart = getSubCycleTime();
      uint8_t sctMinRunTime = sctStart + 4; // Min run time 32ms to avoid false readings.
      uint8_t sct;
      while(!(currentHigh = hw->isCurrentHigh(HardwareMotorDriverInterface::motorDriveOpening)) && ((sct = getSubCycleTime()) <= ((3*GSCT_MAX)/4)))
        { 
        // Wait until end of tick or minimum period.
        if(sct < sctMinRunTime) { while(getSubCycleTime() <= sctMinRunTime) { } }
        else { while(getSubCycleTime() == sct) { } }
        }
      // Stop motor until next loop (also ensures power off).
      hw->motorRun(HardwareMotorDriverInterface::motorOff, *this);
      // Once end-stop has been hit, move to state to wait for user signal and then start calibration. 
if(currentHigh) { DEBUG_SERIAL_PRINTLN_FLASHSTRING("    currentHigh"); }
      if(currentHigh) { state = valvePinWithdrawn; }
      break;
      }

    // Running (initial) calibration cycle.
    case valvePinWithdrawn:
      {
DEBUG_SERIAL_PRINTLN_FLASHSTRING("  valvePinWithdrawn");
      // TODO

      // TEMPORARILY ... RUN BACK TO START AND CYCLE ...
      endStopDetected = false; // Clear the end-stop detection flag ready.
      bool currentHigh = false;
      hw->motorRun(HardwareMotorDriverInterface::motorDriveClosing, *this, true);
      uint8_t sctStart = getSubCycleTime();
      uint8_t sctMinRunTime = sctStart + 4; // Min run time 32ms to avoid false readings.
      uint8_t sct;
      while(!(currentHigh = hw->isCurrentHigh(HardwareMotorDriverInterface::motorDriveClosing)) && ((sct = getSubCycleTime()) <= ((3*GSCT_MAX)/4)))
        { 
        // Wait until end of tick or minimum period.
        if(sct < sctMinRunTime) { while(getSubCycleTime() <= sctMinRunTime) { } }
        else { while(getSubCycleTime() == sct) { } }
        }
      // Stop motor until next loop (also ensures power off).
      hw->motorRun(HardwareMotorDriverInterface::motorOff, *this);
      if(currentHigh) { state = valveCalibrating; }

      break;
      }

    // Running (initial) calibration cycle.
    case valveCalibrating:
DEBUG_SERIAL_PRINTLN_FLASHSTRING("  valveCalibrating");
      // TODO
      break;

    // Unexpected: go to error state, stop motor and panic.
    default:
      {
      state = valveError;
      hw->motorRun(HardwareMotorDriverInterface::motorOff, *this);
      panic(); // Not expected to return.
      return;
      }
    }
  }


















#if defined(ENABLE_BOILER_HUB)
// Boiler output control.

// Set thresholds for per-value and minimum-aggregate percentages to fire the boiler.
// Coerces values to be valid:
// minIndividual in range [1,100] and minAggregate in range [minIndividual,100].
void OnOffBoilerDriverLogic::setThresholds(const uint8_t minIndividual, const uint8_t minAggregate)
  {
  minIndividualPC = constrain(minIndividual, 1, 100);
  minAggregatePC = constrain(minAggregate, minIndividual, 100);
  }

// Called upon incoming notification of status or call for heat from given (valid) ID.
// ISR-/thread- safe to allow for interrupt-driven comms, and as quick as possible.
// Returns false if the signal is rejected, eg from an unauthorised ID.
// The basic behaviour is that a signal with sufficient percent open
// is good for 2 minutes (120s, 60 ticks) unless explicitly cancelled earler,
// for all valve types including FS20/FHT8V-style.
// That may be slightly adjusted for IDs that indicate FS20 housecodes, etc.
//   * id  is the two-byte ID or house code; 0xffffu is never valid
//   * percentOpen  percentage open that the remote valve is reporting
bool OnOffBoilerDriverLogic::receiveSignal(const uint16_t id, const uint8_t percentOpen)
  {
  if((badID == id) || (percentOpen > 100)) { return(false); } // Reject bad args.

  bool accepted = false;

  // Under lock to be ISR-safe.
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
#if defined(BOILER_RESPOND_TO_SPECIFIED_IDS_ONLY)
    // Reject unrecognised IDs if any in the auth list with false return.
    if(badID != authedIDs[0])
      {
        // TODO
      }
#endif


// Find current entry in list if present and update,
// else extend list if possible,
// or replace 0 entry if available to make space,
// or replace lower/lowest entry if this passed 'individual' threshold,
// else reject update.

    // Find entry for current ID if present or create one at end if space,
    // but note in passing lowest % lower than current signal in case above not possible.


    }

  return(accepted);
  }

#if defined(OnOffBoilerDriverLogic_CLEANUP)
// Do some incremental clean-up to speed up future operations.
// Aim to free up at least one status slot if possible.
void OnOffBoilerDriverLogic::cleanup()
  {
  // Swap more-recently-heard-from items towards lower indexes.
  // Kill off trailing old entries.
  // Don't necessarily run on every tick:
  // possibly only run when something actually expires or when out of space.
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    if(badID != status[0].id)
      {
      // TODO
      }
    }
  }
#endif

// Fetches statuses of valves recently heard from and returns the count; 0 if none.
// Optionally filters to return only those still live and apparently calling for heat.
//   * valves  array to copy status to the start of; never null
//   * size  size of valves[] in entries (not bytes), no more entries than that are used,
//     and no more than maxRadiators entries are ever needed
//   * onlyLiveAndCallingForHeat  if true retrieves only current entries
//     'calling for heat' by percentage
uint8_t OnOffBoilerDriverLogic::valvesStatus(PerIDStatus valves[], const uint8_t size, const bool onlyLiveAndCallingForHeat) const
  {
  uint8_t result = 0;
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    for(volatile const PerIDStatus *p = status; (p < status+maxRadiators) && (badID != p->id); ++p)
      {
      // Stop if retun array full.
      if(result >= size) { break; }
      // Skip if filtering and current item not of interest.
      if(onlyLiveAndCallingForHeat && ((p->ticksUntilOff < 0) || (p->percentOpen < minIndividualPC))) { continue; }
      // Copy data into result array and increment count.
      valves[result++] = *(PerIDStatus *)p;
      }
    }
  return(result);
  }

// Poll every 2 seconds in real/virtual time to update state in particular the callForHeat value.
// Not to be called from ISRs,
// in part because this may perform occasional expensive-ish operations
// such as incremental clean-up.
// Because this does not assume a tick is in real time
// this remains entirely unit testable,
// and no use of wall-clack time is made within this or sibling class methods.
void OnOffBoilerDriverLogic::tick2s()
  {
  bool doCleanup = false;

  // If individual and aggregate limits are passed by set of IDs (and minimumTicksUntilOn is zero)
  // then nominally turn boiler on else nominally turn it off.
  // Such change of state may be prevented/delayed by duty-cycle limits.
  //
  // Adjust all expiry timers too.
  //
  // Be careful not to lock out interrupts (ie hold the lock) too long.

  // Set true if at least one valve has met/passed the individual % threshold to be considered calling for heat.
  bool atLeastOneValceCallingForHeat = false;
  // Partial cumulative percent open (stops accumulating once threshold has been passed).
  uint8_t partialCumulativePC = 0;
  ATOMIC_BLOCK (ATOMIC_RESTORESTATE)
    {
    for(volatile PerIDStatus *p = status; (p < status+maxRadiators) && (badID != p->id); ++p)
      {
      // Decrement time-until-expiry until lower limit is reached, at which point call for a cleanup!
      volatile int8_t &t = p->ticksUntilOff;
      if(t > -128) { --t; } else { doCleanup = true; continue; }
      // Ignore stale entries for boiler-state calculation.
      if(t < 0) { continue; }
      // Check if at least one valve is really open.
      if(!atLeastOneValceCallingForHeat && (p->percentOpen >= minIndividualPC)) { atLeastOneValceCallingForHeat = true; }
      // Check if aggregate limits are being reached.
      if(partialCumulativePC < minAggregatePC) { partialCumulativePC += p->percentOpen; }
      }
    }
  // Compute desired boiler state unconstrained by duty-cycle limits.
  // Boiler should be on if both individual and aggregate limits are met.
  const bool desiredBoilerState = atLeastOneValceCallingForHeat && (partialCumulativePC >= minAggregatePC);

#if defined(OnOffBoilerDriverLogic_CLEANUP)
  if(doCleanup) { cleanup(); }
#endif

  // Note passage of a tick in current state.
  if(ticksInCurrentState < 0xff) { ++ticksInCurrentState; }

  // If already in the correct state then nothing to do.
  if(desiredBoilerState == callForHeat) { return; }

  // If not enough ticks have passed to change state then don't.
  if(ticksInCurrentState < minTicksInEitherState) { return; }

  // Change boiler state and reset counter.
  callForHeat = desiredBoilerState;
  ticksInCurrentState = 0;
  }

uint8_t BoilerDriver::read()
   {
   logic.tick2s();
   value = (logic.isCallingForHeat()) ? 0 : 100;
   return(value);
   }

// Singleton implementation/instance.
extern BoilerDriver BoilerControl;
#endif
