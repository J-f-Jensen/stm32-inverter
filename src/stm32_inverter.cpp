/*
 * This file is part of the tumanako_vc project.
 *
 * Copyright (C) 2010 Johannes Huebner <contact@johanneshuebner.com>
 * Copyright (C) 2010 Edward Cheeseman <cheesemanedward@gmail.com>
 * Copyright (C) 2009 Uwe Hermann <uwe@hermann-uwe.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdint.h>
#include <libopencm3/stm32/usart.h>
#include <libopencm3/stm32/timer.h>
#include <libopencm3/stm32/rtc.h>
#include <libopencm3/stm32/can.h>
#include <libopencm3/stm32/iwdg.h>
#include "stm32_can.h"
#include "terminal.h"
#include "params.h"
#include "hwdefs.h"
#include "digio.h"
#include "sine_core.h"
#include "fu.h"
#include "hwinit.h"
#include "anain.h"
#include "temp_meas.h"
#include "param_save.h"
#include "inc_encoder.h"
#include "my_math.h"
#include "errormessage.h"
#include "pwmgeneration.h"
#include "printf.h"
#include "stm32scheduler.h"

#define RMS_SAMPLES 256
#define SQRT2OV1 0.707106781187
#define PRECHARGE_TIMEOUT 500 //5s
#define CAN_TIMEOUT       50  //500ms

HWREV hwRev; //Hardware variant of board we are running on

//Precise control of executing the boost controller
static Stm32Scheduler* scheduler;

static Can* can;
static s32fp torquePercent = 0;
static int CanMessageTimeCounter = 0;

static void GetDigInputs()
{
   static bool canIoActive = false;
   int canio = Param::GetInt(Param::canio);

   canIoActive |= canio != 0;

   if ((rtc_get_counter_val() - can->GetLastRxTimestamp()) >= CAN_TIMEOUT && canIoActive)
   {
      canio = 0;
      Param::SetInt(Param::canio, 0);
      ErrorMessage::Post(ERR_CANTIMEOUT);
   }
   
   Param::SetInt(Param::din_start, DigIo::start_in.Get()); // Used as inverter enable PIN
   Param::SetInt(Param::din_mprot, DigIo::mprot_in.Get());
   Param::SetInt(Param::din_emcystop, DigIo::emcystop_in.Get()); // Optional


   if (hwRev != HW_REV1 && hwRev != HW_BLUEPILL)
   {
      Param::SetInt(Param::din_ocur, DigIo::ocur_in.Get());
      Param::SetInt(Param::din_desat, DigIo::desat_in.Get());
   }
}

static void GetTemps(s32fp& tmphs, s32fp &tmpm)
{
   if (hwRev == HW_TESLA)
   {
      static s32fp hsTemps[3];
      static s32fp mTemps[2];
      static bool isLdu = false;

      int input = DigIo::temp0_out.Get() + 2 * DigIo::temp1_out.Get();
      int tmphsi = AnaIn::tmphs.Get();
      int tmpmi = AnaIn::tmpm.Get();

      tmphs = Param::Get(Param::tmphs); //default to last value;
      tmpm = Param::Get(Param::tmpm);

      switch (input)
      {
         case 0:
            //Mux1.0 reads fluid temperature on both drive units which we ignore
            //Mux2.0 reads inverter temperature A on both drive units
            hsTemps[0] = TempMeas::Lookup(tmphsi, TempMeas::TEMP_TESLA_52K);
            DigIo::temp0_out.Set();
            DigIo::temp1_out.Clear(); //Switch mux for next round
            break;
         case 1:
            //Mux1.1 reads stator temperature and SDU and fluid temperature on LDU which we ignore
            //Mux2.1 reads inverter temperature B on both drive units
            hsTemps[1] = TempMeas::Lookup(tmphsi, TempMeas::TEMP_TESLA_52K);
            if (!isLdu)
               mTemps[0] = TempMeas::Lookup(tmpmi, TempMeas::TEMP_TESLA_100K);
            DigIo::temp0_out.Clear();
            DigIo::temp1_out.Set(); //Switch mux for next round
            break;
         case 2:
            //Mux1.2 reads case temperature on SDU and stator temperature 1 on LDU
            //Mux2.2 reads inverter temperature C on both drive units
            hsTemps[2] = TempMeas::Lookup(tmphsi, TempMeas::TEMP_TESLA_52K);
            if (isLdu)
               mTemps[0] = TempMeas::Lookup(tmpmi, TempMeas::TEMP_TESLA_100K);
            else
               mTemps[1] = TempMeas::Lookup(tmpmi, TempMeas::TEMP_TESLA_10K);
            //Done reading inverter temps, update to maximum
            tmphs = MAX(hsTemps[0], MAX(hsTemps[1], hsTemps[2]));
            DigIo::temp0_out.Set();
            DigIo::temp1_out.Set(); //Switch mux for next round
            break;
         case 3:
            //Mux 1.3 is grounded on SDU and reads stator temperature 2 on LDU
            //Mux 2.3 is grounded on both drive units
            isLdu = tmpmi > 50;  //Tied to GND on SDU
            if (isLdu)
               mTemps[1] = TempMeas::Lookup(tmpmi, TempMeas::TEMP_TESLA_100K);
            //Now update to maximum temperaure
            tmpm = MAX(mTemps[0], mTemps[1]);
            DigIo::temp0_out.Clear();
            DigIo::temp1_out.Clear(); //Switch mux for next round
            break;
      }
   }
   else
   {
      TempMeas::Sensors snshs = (TempMeas::Sensors)Param::GetInt(Param::snshs);
      TempMeas::Sensors snsm = (TempMeas::Sensors)Param::GetInt(Param::snsm);

      int tmphsi = AnaIn::tmphs.Get();
      int tmpmi = AnaIn::tmpm.Get();

      tmpm = TempMeas::Lookup(tmpmi, snsm);

      if (hwRev == HW_PRIUS)
      {
         static s32fp priusTempCoeff = FP_FROMFLT(18.62);

         //We need to install a 10k pull-down resistor to be able
         //to measure temps lower than 56°C. If this resistor
         //is not installed, we will see readings above 2.5V
         //and we double the coefficient to at least get a valid
         //reading.
         if (priusTempCoeff < FP_FROMFLT(20) && tmphsi > 3200)
         {
            priusTempCoeff *= 2;
         }

         tmphs = FP_FROMFLT(166.66) - FP_DIV(FP_FROMINT(tmphsi), priusTempCoeff);
      }
      else
      {
         tmphs = TempMeas::Lookup(tmphsi, snshs);
      }
   }
}

static void CalcAndOutputTemp()
{
   static int temphsFlt = 0;
   static int tempmFlt = 0;
   int pwmgain = Param::GetInt(Param::pwmgain);
   int pwmofs = Param::GetInt(Param::pwmofs);
   int pwmfunc = Param::GetInt(Param::pwmfunc);
   int tmpout = 0;
   s32fp tmphs = 0, tmpm = 0;

   GetTemps(tmphs, tmpm);

   temphsFlt = IIRFILTER(tmphs, temphsFlt, 15);
   tempmFlt = IIRFILTER(tmpm, tempmFlt, 18);

   switch (pwmfunc)
   {
      default:
      case PWM_FUNC_TMPM:
         tmpout = FP_TOINT(tmpm) * pwmgain + pwmofs;
         break;
      case PWM_FUNC_TMPHS:
         tmpout = FP_TOINT(tmphs) * pwmgain + pwmofs;
         break;
      case PWM_FUNC_SPEED:
         tmpout = Param::Get(Param::speed) * pwmgain + pwmofs;
         break;
      case PWM_FUNC_SPEEDFRQ:
         //Handled in 1ms task
         break;
   }

   tmpout = MIN(0xFFFF, MAX(0, tmpout));

   timer_set_oc_value(OVER_CUR_TIMER, TIM_OC4, tmpout);

   Param::SetFlt(Param::tmphs, tmphs);
   Param::SetFlt(Param::tmpm, tmpm);
}

static s32fp ProcessUdc()
{
   static int32_t udc = 0;
   s32fp udcfp;
   s32fp udcmin = Param::Get(Param::udcmin);
   s32fp udcmax = Param::Get(Param::udcmax);
   s32fp udclim = Param::Get(Param::udclim);
   s32fp udcgain = Param::Get(Param::udcgain);
   s32fp udcsw = Param::Get(Param::udcsw);
   int udcofs = Param::GetInt(Param::udcofs);

   //Calculate "12V" supply voltage from voltage divider on mprot pin
   //1.2/(4.7+1.2)/3.33*4095 = 250 -> make it a bit less for pin losses etc
   //HW_REV1 had 3.9k resistors
   int uauxGain = hwRev == HW_REV1 ? 289 : 249;
   Param::SetFlt(Param::uaux, FP_DIV(AnaIn::uaux.Get(), uauxGain));
   udc = IIRFILTER(udc, AnaIn::udc.Get(), 2);
   udcfp = FP_DIV(FP_FROMINT(udc - udcofs), udcgain);

   if (hwRev != HW_TESLAM3)
   {
      if (udcfp < udcmin || udcfp > udcmax)
         DigIo::vtg_out.Set();
      else
         DigIo::vtg_out.Clear();
   }

   if (udcfp > udclim)
   {
      if (Encoder::GetSpeed() < 50) //If motor is stationary, over voltage comes from outside
      {
         DigIo::dcsw_out.Clear();  //In this case, open DC switch
         DigIo::prec_out.Clear();  //and precharge
      }

      Param::SetInt(Param::opmode, MOD_OFF);
      DigIo::err_out.Set();
      ErrorMessage::Post(ERR_OVERVOLTAGE);
   }

   if (udcfp < (udcsw / 2) && rtc_get_counter_val() > PRECHARGE_TIMEOUT && DigIo::prec_out.Get())
   {
      DigIo::err_out.Set();
      DigIo::prec_out.Clear();
      ErrorMessage::Post(ERR_PRECHARGE);
   }

   #if CONTROL == CTRL_SINE
   {
      s32fp udcnom = Param::Get(Param::udcnom);
      s32fp fweak = Param::Get(Param::fweak);
      s32fp boost = Param::Get(Param::boost);

      if (udcnom > 0)
      {
         s32fp udcdiff = udcfp - udcnom;
         s32fp factor = FP_FROMINT(1) + FP_DIV(udcdiff, udcnom);
         //increase fweak on voltage above nominal
         fweak = FP_MUL(fweak, factor);
         //decrease boost on voltage below nominal
         boost = FP_DIV(boost, factor);
      }

      Param::SetFlt(Param::fweakcalc, fweak);
      Param::SetFlt(Param::boostcalc, boost);
      MotorVoltage::SetWeakeningFrq(fweak);
      MotorVoltage::SetBoost(FP_TOINT(boost));
   }
   #endif // CONTROL

   Param::SetFlt(Param::udc, udcfp);

   return udcfp;
}

static void Ms1Task(void)
{
   /*
   static int speedCnt = 0;

   if (Param::GetInt(Param::pwmfunc) == PWM_FUNC_SPEEDFRQ)
   {
      int speed = Param::GetInt(Param::speed);
      if (speedCnt == 0 && speed != 0)
      {
         DigIo::speed_out.Toggle();
         speedCnt = Param::GetInt(Param::pwmgain) / (2 * speed);
      }
      else if (speedCnt > 0)
      {
         speedCnt--;
      }
   }
   */
}

//Normal run takes 70µs -> 0.7% cpu load (last measured version 3.5)
static void Ms10Task(void)
{
   static int initWait = 0;
   static s32fp chargeCurRamped = 0;
   int opmode = Param::GetInt(Param::opmode);
   int chargemode = Param::GetInt(Param::chargemode);
   int newMode = MOD_OFF;
   int stt = STAT_NONE;
   s32fp udc = ProcessUdc();

   ErrorMessage::SetTime(rtc_get_counter_val());
   Encoder::UpdateRotorFrequency(100);
   GetDigInputs();
   CalcAndOutputTemp();
   Param::SetInt(Param::speed, Encoder::GetSpeed());

   if (MOD_RUN == opmode && initWait == -1)
   {
      PwmGeneration::SetTorquePercent(torquePercent);
   }

   stt |= DigIo::emcystop_in.Get() ? STAT_NONE : STAT_EMCYSTOP;
   stt |= DigIo::mprot_in.Get() ? STAT_NONE : STAT_MPROT;
   stt |= udc >= Param::Get(Param::udcsw) ? STAT_NONE : STAT_UDCBELOWUDCSW;
   stt |= udc < Param::Get(Param::udclim) ? STAT_NONE : STAT_UDCLIM;

   /* switch to runmode if
    * - start pin is high
    * - motor protection and emcystop is high (=inactive)
    * - udc >= udcsw
    * - udc < udclim
    */
   if ((stt & (STAT_EMCYSTOP | STAT_MPROT | STAT_POTPRESSED | STAT_UDCBELOWUDCSW | STAT_UDCLIM)) == STAT_NONE)
   {
	  if (Param::GetBool(Param::din_start) ||
      	  (Param::GetInt(Param::tripmode) == TRIP_AUTORESUME && PwmGeneration::Tripped()))
	  {
		newMode = MOD_RUN;
	  }
      stt |= opmode != MOD_OFF ? STAT_NONE : STAT_WAITSTART;
   }

   Param::SetInt(Param::status, stt);

   if (newMode != MOD_OFF)
   {
      Param::SetInt(Param::opmode, newMode);
      ErrorMessage::UnpostAll();
   }

   if (hwRev != HW_TESLA && opmode >= MOD_BOOST && Param::GetBool(Param::din_bms))
   {
      opmode = MOD_OFF;
      Param::SetInt(Param::opmode, opmode);
   }

   if (MOD_OFF == opmode)
   {
      initWait = 50;

      PwmGeneration::SetTorquePercent(0);
      PwmGeneration::SetOpmode(MOD_OFF);
	  
	  torquePercent=0;
   }
   else if (0 == initWait)
   {
      torquePercent=0;
      Encoder::Reset();
      //this applies new deadtime and pwmfrq and enables the outputs for the given mode
      PwmGeneration::SetOpmode(opmode);
      DigIo::err_out.Clear();
      if (hwRev == HW_TESLAM3)
         DigIo::vtg_out.Clear();
      initWait = -1;
   }
   else if (initWait == 10)
   {
      PwmGeneration::SetCurrentOffset(AnaIn::il1.Get(), AnaIn::il2.Get());
      if (hwRev == HW_TESLAM3)
         DigIo::vtg_out.Set();
      initWait--;
   }
   else if (initWait > 0)
   {
      initWait--;
   }

   if (Param::GetInt(Param::canperiod) == CAN_PERIOD_10MS)
      can->SendAll();
}

static void Ms100Task(void)
{
   DigIo::led_out.Toggle();
   iwdg_reset();
   s32fp cpuLoad = FP_FROMINT(PwmGeneration::GetCpuLoad() + scheduler->GetCpuLoad());
   Param::SetFlt(Param::cpuload, cpuLoad / 10);
   Param::SetInt(Param::turns, Encoder::GetFullTurns());
   Param::SetInt(Param::lasterr, ErrorMessage::GetLastError());

   // If we not have recived can message within the last 400~500ms then we stop the inverter
   if (CanMessageTimeCounter == 0)
   {
       Param::SetInt(Param::opmode, MOD_OFF);
       torquePercent = 0;
   }
   else 
   {
       CanMessageTimeCounter--;
   }

   if (hwRev == HW_REV1 || hwRev == HW_BLUEPILL)
   {
      //If mprot is high than it must be over current
      if (DigIo::mprot_in.Get())
      {
         Param::SetInt(Param::din_ocur, 0);
      }
      else
      {
         Param::SetInt(Param::din_ocur, 1);
      }
      Param::SetInt(Param::din_desat, 2);
   }

   #if CONTROL == CTRL_SINE
   //uac = udc * amp/maxamp / sqrt(2)
   s32fp uac = Param::Get(Param::udc) * SineCore::GetAmp();
   uac /= SineCore::MAXAMP;
   uac = FP_DIV(uac, FP_FROMFLT(1.4142));

   Param::SetFlt(Param::uac, uac);
   #endif // CONTROL

   if (Param::GetInt(Param::canperiod) == CAN_PERIOD_100MS)
      can->SendAll();
}

/** This function is called when the user changes a parameter */
extern void parm_Change(Param::PARAM_NUM paramNum)
{
   switch (paramNum)
   {
   #if CONTROL == CTRL_SINE
      case Param::fslipspnt:
         PwmGeneration::SetFslip(Param::Get(Param::fslipspnt));
         break;
      case Param::ampnom:
         PwmGeneration::SetAmpnom(Param::Get(Param::ampnom));
         break;
   #endif
      case Param::canspeed:
         can->SetBaudrate((Can::baudrates)Param::GetInt(Param::canspeed));
         break;
      case Param::throtmax:
      case Param::throtmin:
      case Param::idcmin:
      case Param::idcmax:
         //These are candidates to be frequently set by CAN, so we handle them separately
/*         Throttle::throtmax = Param::Get(Param::throtmax);
         Throttle::throtmin = Param::Get(Param::throtmin);
         Throttle::idcmin = Param::Get(Param::idcmin);
         Throttle::idcmax = Param::Get(Param::idcmax); */
         break;
      default:
         PwmGeneration::SetCurrentLimitThreshold(Param::Get(Param::ocurlim));
         PwmGeneration::SetPolePairRatio(Param::GetInt(Param::polepairs) / Param::GetInt(Param::respolepairs));

         #if CONTROL == CTRL_FOC
         PwmGeneration::SetControllerGains(Param::GetInt(Param::curkp), Param::GetInt(Param::curki), Param::GetInt(Param::fwkp));
         Encoder::SwapSinCos((Param::GetInt(Param::pinswap) & SWAP_RESOLVER) > 0);
         #elif CONTROL == CTRL_SINE
         MotorVoltage::SetMinFrq(FP_FROMFLT(0.2));
         SineCore::SetMinPulseWidth(1000);
         #endif // CONTROL

         Encoder::SetMode((enum Encoder::mode)Param::GetInt(Param::encmode));
         Encoder::SetImpulsesPerTurn(Param::GetInt(Param::numimp));
/*
         Throttle::potmin[0] = Param::GetInt(Param::potmin);
         Throttle::potmax[0] = Param::GetInt(Param::potmax);
         Throttle::potmin[1] = Param::GetInt(Param::pot2min);
         Throttle::potmax[1] = Param::GetInt(Param::pot2max);
         Throttle::brknom = Param::Get(Param::brknom);
         Throttle::brknompedal = Param::Get(Param::brknompedal);
         Throttle::regenRamp = Param::Get(Param::regenramp);
         Throttle::brkmax = Param::Get(Param::brkmax);
         Throttle::brkcruise = Param::Get(Param::brkcruise);
         Throttle::throtmax = Param::Get(Param::throtmax);
         Throttle::throtmin = Param::Get(Param::throtmin);
         Throttle::idleSpeed = Param::GetInt(Param::idlespeed);
         Throttle::speedkp = Param::Get(Param::speedkp);
         Throttle::speedflt = Param::GetInt(Param::speedflt);
         Throttle::idleThrotLim = Param::Get(Param::idlethrotlim);
         Throttle::bmslimlow = Param::GetInt(Param::bmslimlow);
         Throttle::bmslimhigh = Param::GetInt(Param::bmslimhigh);
         Throttle::udcmin = FP_MUL(Param::Get(Param::udcmin), FP_FROMFLT(0.95)); //Leave some room for the notification light
         Throttle::udcmax = FP_MUL(Param::Get(Param::udcmax), FP_FROMFLT(1.05));
         Throttle::idcmin = Param::Get(Param::idcmin);
         Throttle::idcmax = Param::Get(Param::idcmax);
         Throttle::fmax = Param::Get(Param::fmax);
*/
         if (hwRev != HW_BLUEPILL)
         {
            if (Param::GetInt(Param::pwmfunc) == PWM_FUNC_SPEEDFRQ)
               gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_PUSHPULL, GPIO9);
            else
               gpio_set_mode(GPIOB, GPIO_MODE_OUTPUT_50_MHZ, GPIO_CNF_OUTPUT_ALTFN_PUSHPULL, GPIO9);
         }
         break;
   }
}

static void InitPWMIO()
{
   uint8_t outputMode = Param::GetInt(Param::pwmpol) == 0 ? GPIO_MODE_OUTPUT_50_MHZ : GPIO_MODE_INPUT;
   uint8_t outputConf = Param::GetInt(Param::pwmpol) == 0 ? GPIO_CNF_OUTPUT_ALTFN_PUSHPULL : GPIO_CNF_INPUT_FLOAT;
   uint16_t actualPattern = gpio_get(GPIOA, GPIO8 | GPIO9 | GPIO10) | gpio_get(GPIOB, GPIO13 | GPIO14 | GPIO15);

   Param::SetInt(Param::pwmio, actualPattern);

   gpio_set_mode(GPIOA, outputMode, outputConf, GPIO8 | GPIO9 | GPIO10);
   gpio_set_mode(GPIOB, outputMode, outputConf, GPIO13 | GPIO14 | GPIO15);
}

static void ConfigureVariantIO()
{
   hwRev = detect_hw();
   Param::SetInt(Param::hwver, hwRev);

   ANA_IN_CONFIGURE(ANA_IN_LIST);
   DIG_IO_CONFIGURE(DIG_IO_LIST);

   switch (hwRev)
   {
      case HW_REV1:
         AnaIn::il2.Configure(GPIOA, 6);
         break;
      case HW_REV2:
      case HW_REV3:
      case HW_TESLAM3:
         break;
      case HW_PRIUS:
         DigIo::emcystop_in.Configure(GPIOC, GPIO7, PinMode::INPUT_PU);
         break;
      case HW_TESLA:
         DigIo::temp1_out.Configure(GPIOC, GPIO8, PinMode::OUTPUT);
         //Essentially disable error output by mapping it to an unused pin
         DigIo::err_out.Configure(GPIOB, GPIO9, PinMode::INPUT_FLT);
         break;
      case HW_BLUEPILL:
         ANA_IN_CONFIGURE(ANA_IN_LIST_BLUEPILL);
         DIG_IO_CONFIGURE(DIG_IO_BLUEPILL);
         break;
   }

   AnaIn::Start();
}

extern "C" void tim2_isr(void)
{
   scheduler->Run();
}

extern "C" void tim4_isr(void)
{
   scheduler->Run();
}

static void ProcessCan0x287Message(uint32_t data[2])
{
    int opmode = Param::GetInt(Param::opmode);

    uint16_t rawCanTorquePercent = data[0] >> 16 & 0xFFFF;
    uint8_t rawCanStatus = data[1] >> 8 & 0xFF;

    CanMessageTimeCounter = 4;

    if ( rawCanStatus == 0x03 )
    {
        torquePercent = rawCanTorquePercent / 10;
        opmode = MOD_RUN;
    }
    else 
    {
        torquePercent = 0;
        opmode = MOD_OFF;
    }

    Param::SetInt(Param::opmode, opmode);
}

static void CanCallback(uint32_t id, uint32_t data[2])
{
   switch (id)
   {
   case 0x287:
      ProcessCan0x287Message(data);
      break;

   default:
      break;
   }
}

extern "C" int main(void)
{
   clock_setup();
   rtc_setup();
   ConfigureVariantIO();
   write_bootloader_pininit();

   //Additional test pins on JTAG header
   //AFIO_MAPR |= AFIO_MAPR_SPI1_REMAP | AFIO_MAPR_SWJ_CFG_JTAG_OFF_SW_OFF;
   usart_setup();
   tim_setup();
   nvic_setup();
   Encoder::Reset();
   term_Init();
   parm_load();
   parm_Change(Param::PARAM_LAST);
   ErrorMessage::SetTime(1);
   InitPWMIO();

   MotorVoltage::SetMaxAmp(SineCore::MAXAMP);

   Stm32Scheduler s(hwRev == HW_BLUEPILL ? TIM4 : TIM2); //We never exit main so it's ok to put it on stack
   scheduler = &s;
   
   Can c(CAN1, (Can::baudrates)Param::GetInt(Param::canspeed));
   c.SetReceiveCallback(CanCallback);
   c.RegisterUserMessage(0x287); // We only listen for messages on this address 
   can = &c;

   s.AddTask(Ms1Task, 1);
   s.AddTask(Ms10Task, 10);
   s.AddTask(Ms100Task, 100);

   DigIo::prec_out.Set();

   Param::SetInt(Param::version, 4); //backward compatibility

   if (Param::GetInt(Param::snsm) < 12)
      Param::SetInt(Param::snsm, Param::GetInt(Param::snsm) + 10); //upgrade parameter
   if (Param::Get(Param::brkmax) > 0)
      Param::Set(Param::brkmax, -Param::Get(Param::brkmax));

   term_Run();

   return 0;
}

