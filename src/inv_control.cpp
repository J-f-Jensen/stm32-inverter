#include <libopencm3/stm32/rtc.h>
#include "inv_control.h"
#include "throttle.h"
#include "digio.h"
#include "params.h"
#include "param_save.h"
#include "my_math.h"
#include "inc_encoder.h"
#include "anain.h"

static int CallNumber=0;

int inv_control::RunInvControl()
{
   CallNumber++;
   
   // Read Digital Ports
   Param::SetInt(Param::din_cruise, DigIo::cruise_in.Get());
   Param::SetInt(Param::din_brake, DigIo::brake_in.Get());
   Param::SetInt(Param::din_forward, DigIo::fwd_in.Get());
   Param::SetInt(Param::din_reverse, DigIo::rev_in.Get());
   //Param::SetInt(Param::din_bms, (Param::canio & CAN_IO_BMS) != 0 || (hwRev == HW_TESLA ? false : DigIo::bms_in.Get()) );
   Param::SetInt(Param::din_bms, DigIo::bms_in.Get() );
   
   if (CallNumber == 10)
   {
	   // Run at evert 10't call
	   SelectDirection();
	   CruiseControl();
	   
	   CallNumber=0;
   }
   
   return ProcessThrottle();
}

void inv_control::ResetInvControl()
{
	Throttle::RampThrottle(0); //Restart ramp
	Throttle::cruiseSpeed = -1;
}


void inv_control::PostErrorIfRunning(ERROR_MESSAGE_NUM err)
{
   if (Param::GetInt(Param::opmode) == MOD_RUN)
   {
      ErrorMessage::Post(err);
   }
}

void inv_control::CruiseControl()
{
   static bool lastState = false;

   //Always disable cruise control when brake pedal is pressed
   if (Param::GetBool(Param::din_brake))
   {
      Throttle::cruiseSpeed = -1;
   }
   else
   {
      if (Param::GetInt(Param::cruisemode) == CRUISE_BUTTON)
      {
         //Enable/update cruise control when button is pressed
         if (Param::GetBool(Param::din_cruise))
         {
            Throttle::cruiseSpeed = Encoder::GetSpeed();
         }
      }
      else if (Param::GetInt(Param::cruisemode) == CRUISE_SWITCH)
      {
         //Enable/update cruise control when switch is toggled on
         if (Param::GetBool(Param::din_cruise) && !lastState)
         {
            Throttle::cruiseSpeed = Encoder::GetSpeed();
         }

         //Disable cruise control when switch is off
         if (!Param::GetBool(Param::din_cruise))
         {
            Throttle::cruiseSpeed = -1;
         }
      }
      else if (Param::GetInt(Param::cruisemode) == CRUISE_CAN)
      {
         Throttle::cruiseSpeed = Param::GetInt(Param::cruisespeed);
      }
   }

   if (Param::GetInt(Param::cruisemode) != CRUISE_CAN)
   {
      Param::SetInt(Param::cruisespeed, Throttle::cruiseSpeed);
   }

   lastState = Param::GetBool(Param::din_cruise);
}

void inv_control::SelectDirection()
{
   int selectedDir = Param::GetInt(Param::dir);
   int userDirSelection = 0;
   int dirSign = (Param::GetInt(Param::dirmode) & DIR_REVERSED) ? -1 : 1;

   if (Param::GetInt(Param::dirmode) == DIR_DEFAULTFORWARD)
   {
      if (Param::GetBool(Param::din_forward) && Param::GetBool(Param::din_reverse))
         selectedDir = 0;
      else if (Param::GetBool(Param::din_reverse))
         userDirSelection = -1;
      else
         userDirSelection = 1;
   }
   else if ((Param::GetInt(Param::dirmode) & 1) == DIR_BUTTON)
   {
      /* if forward AND reverse selected, force neutral, because it's charge mode */
      if (Param::GetBool(Param::din_forward) && Param::GetBool(Param::din_reverse))
         selectedDir = 0;
      else if (Param::GetBool(Param::din_forward))
         userDirSelection = 1 * dirSign;
      else if (Param::GetBool(Param::din_reverse))
         userDirSelection = -1 * dirSign;
      else
         userDirSelection = selectedDir;
   }
   else
   {
      /* neither forward nor reverse or both forward and reverse -> neutral */
      if (!(Param::GetBool(Param::din_forward) ^ Param::GetBool(Param::din_reverse)))
         selectedDir = 0;
      else if (Param::GetBool(Param::din_forward))
         userDirSelection = 1 * dirSign;
      else if (Param::GetBool(Param::din_reverse))
         userDirSelection = -1 * dirSign;
   }

   /* Only change direction when below certain motor speed */
   if ((int)Encoder::GetSpeed() < Param::GetInt(Param::dirchrpm))
      selectedDir = userDirSelection;

   /* Current direction doesn't match selected direction -> neutral */
   if (selectedDir != userDirSelection)
      selectedDir = 0;

   Param::SetInt(Param::dir, selectedDir);
}

int inv_control::GetUserThrottleCommand()
{
   int potval, pot2val;
   bool brake = Param::GetBool(Param::din_brake);
   int potmode = Param::GetInt(Param::potmode);

/*
   if (potmode == POTMODE_CAN)
   {
      //500ms timeout
      if ((rtc_get_counter_val() - can->GetLastRxTimestamp()) < CAN_TIMEOUT)
      {
         potval = Param::GetInt(Param::pot);
         pot2val = Param::GetInt(Param::pot2);
      }
      else
      {
         DigIo::err_out.Set();
         PostErrorIfRunning(ERR_CANTIMEOUT);
         return 0;
      }
   }
   else
   {
*/
      potval = AnaIn::throttle1.Get();
      pot2val = AnaIn::throttle2.Get();
      Param::SetInt(Param::pot, potval);
      Param::SetInt(Param::pot2, pot2val);
 //  }

   /* Error light on implausible value */
   if (!Throttle::CheckAndLimitRange(&potval, 0))
   {
      DigIo::err_out.Set();
      PostErrorIfRunning(ERR_THROTTLE1);
      return 0;
   }

   bool throt2Res = Throttle::CheckAndLimitRange(&pot2val, 1);

   if (potmode == POTMODE_DUALCHANNEL)
   {
      if (!Throttle::CheckDualThrottle(&potval, pot2val) || !throt2Res)
      {
         DigIo::err_out.Set();
         PostErrorIfRunning(ERR_THROTTLE1);
         Param::SetInt(Param::potnom, 0);
         return 0;
      }
      pot2val = Throttle::potmax[1]; //make sure we don't attenuate regen
   }

   if (Param::GetInt(Param::dir) == 0)
      return 0;

   return Throttle::CalcThrottle(potval, pot2val, brake);
}

void inv_control::GetCruiseCreepCommand(s32fp& finalSpnt, s32fp throtSpnt)
{
   bool brake = Param::GetBool(Param::din_brake);
   s32fp idleSpnt = Throttle::CalcIdleSpeed(Encoder::GetSpeed());
   s32fp cruiseSpnt = Throttle::CalcCruiseSpeed(Encoder::GetSpeed());

   finalSpnt = throtSpnt; //assume no regulation

   if (Param::GetInt(Param::idlemode) == IDLE_MODE_ALWAYS ||
      (Param::GetInt(Param::idlemode) == IDLE_MODE_NOBRAKE && !brake) ||
      (Param::GetInt(Param::idlemode) == IDLE_MODE_CRUISE && !brake && Param::GetBool(Param::din_cruise)))
   {
      finalSpnt = MAX(throtSpnt, idleSpnt);
   }

   if (Throttle::cruiseSpeed > 0 && Throttle::cruiseSpeed > Throttle::idleSpeed)
   {
      if (throtSpnt <= 0)
         finalSpnt = cruiseSpnt;
      else if (throtSpnt > 0)
         finalSpnt = MAX(cruiseSpnt, throtSpnt);
   }
}

s32fp inv_control::ProcessThrottle()
{
   s32fp throtSpnt, finalSpnt;

   if ((int)Encoder::GetSpeed() < Param::GetInt(Param::throtramprpm))
      Throttle::throttleRamp = Param::Get(Param::throtramp);
   else
      Throttle::throttleRamp = Param::GetAttrib(Param::throtramp)->max;

   throtSpnt = GetUserThrottleCommand();
   GetCruiseCreepCommand(finalSpnt, throtSpnt);
   finalSpnt = Throttle::RampThrottle(finalSpnt);

   if (hwRev != HW_TESLA)
      Throttle::BmsLimitCommand(finalSpnt, Param::GetBool(Param::din_bms));

   Throttle::UdcLimitCommand(finalSpnt, Param::Get(Param::udc));
   Throttle::IdcLimitCommand(finalSpnt, Param::Get(Param::idc));
   Throttle::FrequencyLimitCommand(finalSpnt, Param::Get(Param::fstat));

   if (Throttle::TemperatureDerate(Param::Get(Param::tmphs), Param::Get(Param::tmphsmax), finalSpnt))
   {
      DigIo::err_out.Set();
      ErrorMessage::Post(ERR_TMPHSMAX);
   }

   if (Throttle::TemperatureDerate(Param::Get(Param::tmpm), Param::Get(Param::tmpmmax), finalSpnt))
   {
      DigIo::err_out.Set();
      ErrorMessage::Post(ERR_TMPMMAX);
   }

   Param::SetFlt(Param::potnom, finalSpnt);

   if (finalSpnt < Param::Get(Param::brkout))
      DigIo::brk_out.Set();
   else
      DigIo::brk_out.Clear();

   return finalSpnt;
}