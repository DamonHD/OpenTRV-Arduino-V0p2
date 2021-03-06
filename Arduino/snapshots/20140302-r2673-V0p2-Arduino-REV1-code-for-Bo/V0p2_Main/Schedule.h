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

Author(s) / Copyright (s): Damon Hart-Davis 2013
*/

/*
 Schedule support for TRV.
 */

#ifndef SCHEDULE_H
#define SCHEDULE_H

#include "V0p2_Main.h"

#include "EEPROM_Utils.h"

// Granularity of simple schedule in minutes (values may be rounded/truncated to nearest); strictly positive.
#define SIMPLE_SCHEDULE_GRANULARITY_MINS 6

// Expose number of supported schedules.
#if defined(BUTTON_LEARN2_L) // With a second learn button up to 2 schedules are supported.
#define MAX_SIMPLE_SCHEDULES (min(2, EE_START_MAX_SIMPLE_SCHEDULES))
#else // Else at most one schedule is supported.
#define MAX_SIMPLE_SCHEDULES (min(1, EE_START_MAX_SIMPLE_SCHEDULES))
#endif

// Get the simple schedule on time, as minutes after midnight [0,1439]; invalid (eg ~0) if none set.
// Note that unprogrammed EEPROM value will result in invalid time, ie schedule not set.
//   * which  schedule number, counting from 0
uint_least16_t getSimpleScheduleOn(uint8_t which);

// Get the simple schedule off time, as minutes after midnight [0,1439]; invalid (eg ~0) if none set.
// This is based on specifed start time and some element of the current eco/comfort bias.
//   * which  schedule number, counting from 0
uint_least16_t getSimpleScheduleOff(uint8_t which);

// Set the simple simple on time.
//   * startMinutesSinceMidnightLT  is start/on time in minutes after midnight [0,1439]
//   * which  schedule number, counting from 0
// Invalid parameters will be ignored and false returned,
// else this will return true and isSimpleScheduleSet() will return true after this.
// NOTE: over-use of this routine can prematurely wear out the EEPROM.
bool setSimpleSchedule(uint_least16_t startMinutesSinceMidnightLT, uint8_t which);

// Clear a simple schedule.
// There will be neither on nor off events from the selected simple schedule once this is called.
//   * which  schedule number, counting from 0
void clearSimpleSchedule(uint8_t which);

// True iff any schedule is 'on'/'WARN' even when schedules overlap.
// Can be used to suppress all 'off' activity except for the final one.
// Can be used to suppress set-backs during on times.
bool isAnyScheduleOnWARMNow();

// True iff any schedule is currently 'on'/'WARM' even when schedules overlap.
// May be relatively slow/expensive.
// Can be used to suppress all 'off' activity except for the final one.
// Can be used to suppress set-backs during on times.
bool isAnySimpleScheduleSet();


#endif


