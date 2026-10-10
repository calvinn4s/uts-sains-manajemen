
#property strict

#include <Trade/Trade.mqh>
CTrade trade;

// ===== STRATEGY SETTINGS =====
input double RiskPerTrade       = 0.25;
input double MaxDrawdownPercent = 25.0;

input int FastEMA                = 50;
input int SlowEMA                = 200;
input int ATRPeriod              = 14;
input int ADXPeriod              = 14;
input int RSIPeriod              = 14;

input double MinADX              = 22.0;
input double SL_ATR              = 2.5;
input double TP_ATR              = 5.0;

input int CooldownBars           = 3;
input int MagicNumber            = 20261009;

// ===== INDICATOR HANDLES =====
int fastHandle = INVALID_HANDLE;
int slowHandle = INVALID_HANDLE;
int atrHandle  = INVALID_HANDLE;
int adxHandle  = INVALID_HANDLE;
int rsiHandle  = INVALID_HANDLE;

// ===== STATE =====
double peakEquity = 0.0;
datetime lastBarTime = 0;
datetime lastTradeBar = 0;
bool tradingStopped = false;

//+------------------------------------------------------------------+
int OnInit()
{
   if(FastEMA <= 0 || SlowEMA <= FastEMA ||
      ATRPeriod <= 0 || ADXPeriod <= 0 ||
      RSIPeriod <= 0 || RiskPerTrade <= 0 ||
      MaxDrawdownPercent <= 0 ||
      SL_ATR <= 0 || TP_ATR <= 0)
   {
      Print("Parameter tidak valid.");
      return INIT_PARAMETERS_INCORRECT;
   }

   fastHandle = iMA(_Symbol, PERIOD_H1, FastEMA, 0,
                    MODE_EMA, PRICE_CLOSE);
   slowHandle = iMA(_Symbol, PERIOD_H1, SlowEMA, 0,
                    MODE_EMA, PRICE_CLOSE);
   atrHandle = iATR(_Symbol, PERIOD_H1, ATRPeriod);
   adxHandle = iADX(_Symbol, PERIOD_H1, ADXPeriod);
   rsiHandle = iRSI(_Symbol, PERIOD_H1, RSIPeriod,
                    PRICE_CLOSE);

   if(fastHandle == INVALID_HANDLE ||
      slowHandle == INVALID_HANDLE ||
      atrHandle == INVALID_HANDLE ||
      adxHandle == INVALID_HANDLE ||
      rsiHandle == INVALID_HANDLE)
   {
      Print("Gagal membuat indikator: ", GetLastError());
      return INIT_FAILED;
   }

   trade.SetExpertMagicNumber(MagicNumber);
   trade.SetTypeFillingBySymbol(_Symbol);

   peakEquity = AccountInfoDouble(ACCOUNT_EQUITY);

   Print("Industrial Grade V3 siap pada ", _Symbol);
   return INIT_SUCCEEDED;
}

//+------------------------------------------------------------------+
void OnDeinit(const int reason)
{
   if(fastHandle != INVALID_HANDLE) IndicatorRelease(fastHandle);
   if(slowHandle != INVALID_HANDLE) IndicatorRelease(slowHandle);
   if(atrHandle  != INVALID_HANDLE) IndicatorRelease(atrHandle);
   if(adxHandle  != INVALID_HANDLE) IndicatorRelease(adxHandle);
   if(rsiHandle  != INVALID_HANDLE) IndicatorRelease(rsiHandle);
}

//+------------------------------------------------------------------+
bool IsNewBar()
{
   datetime barTime = iTime(_Symbol, PERIOD_H1, 0);

   if(barTime <= 0 || barTime == lastBarTime)
      return false;

   lastBarTime = barTime;
   return true;
}

//+------------------------------------------------------------------+
bool HasPosition()
{
   for(int i = PositionsTotal() - 1; i >= 0; i--)
   {
      ulong ticket = PositionGetTicket(i);

      if(ticket == 0)
         continue;

      if(PositionGetString(POSITION_SYMBOL) == _Symbol &&
         PositionGetInteger(POSITION_MAGIC) == MagicNumber)
         return true;
   }

   return false;
}

//+------------------------------------------------------------------+
//| Drawdown from highest observed equity                             |
//+------------------------------------------------------------------+
bool CheckDrawdown()
{
   double equity = AccountInfoDouble(ACCOUNT_EQUITY);

   if(equity > peakEquity)
      peakEquity = equity;

   if(peakEquity <= 0)
      return true;

   double dd = (peakEquity - equity) / peakEquity * 100.0;

   if(dd >= MaxDrawdownPercent)
   {
      tradingStopped = true;

      Print("DRAWDOWN LIMIT: ", DoubleToString(dd, 2), "%");

      // Attempt to close EA positions on this symbol.
      for(int i = PositionsTotal() - 1; i >= 0; i--)
      {
         ulong ticket = PositionGetTicket(i);

         if(ticket == 0)
            continue;

         if(PositionGetString(POSITION_SYMBOL) == _Symbol &&
            PositionGetInteger(POSITION_MAGIC) == MagicNumber)
         {
            if(!trade.PositionClose(ticket))
               Print("Gagal menutup posisi: ",
                     trade.ResultRetcodeDescription());
         }
      }

      return true;
   }

   return false;
}

//+------------------------------------------------------------------+
double CalculateLot(double stopDistance, bool isBuy)
{
   double equity = AccountInfoDouble(ACCOUNT_EQUITY);
   double riskMoney = equity * RiskPerTrade / 100.0;

   double price = isBuy
      ? SymbolInfoDouble(_Symbol, SYMBOL_ASK)
      : SymbolInfoDouble(_Symbol, SYMBOL_BID);

   double stopPrice = isBuy
      ? price - stopDistance
      : price + stopDistance;

   double lossOneLot = 0.0;
   ENUM_ORDER_TYPE type = isBuy ? ORDER_TYPE_BUY : ORDER_TYPE_SELL;

   if(!OrderCalcProfit(type, _Symbol, 1.0,
                       price, stopPrice, lossOneLot))
   {
      Print("OrderCalcProfit gagal: ", GetLastError());
      return 0.0;
   }

   lossOneLot = MathAbs(lossOneLot);

   if(lossOneLot <= 0)
      return 0.0;

   double rawLot = riskMoney / lossOneLot;

   double minLot = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
   double maxLot = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX);
   double step = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP);

   if(step <= 0 || minLot <= 0)
      return 0.0;

   // Never round up beyond the calculated risk.
   double lot = MathFloor(rawLot / step + 1e-9) * step;

   if(lot < minLot)
      return 0.0;

   lot = MathMin(lot, maxLot);

   int digits = 0;
   double testStep = step;

   while(digits < 8 &&
         MathAbs(testStep - MathRound(testStep)) > 1e-8)
   {
      testStep *= 10.0;
      digits++;
   }

   return NormalizeDouble(lot, digits);
}

//+------------------------------------------------------------------+
void OnTick()
{
   if(tradingStopped)
      return;

   if(CheckDrawdown())
      return;

   // Signal evaluation only once per completed H1 candle.
   if(!IsNewBar())
      return;

   if(HasPosition())
      return;

   // Cooldown reduces repeated entries in the same market phase.
   if(lastTradeBar > 0)
   {
      int barsSince = iBarShift(_Symbol, PERIOD_H1,
                                lastTradeBar, true);

      if(barsSince >= 0 && barsSince < CooldownBars)
         return;
   }

   double fast[1], slow[1], atr[1], adx[1], rsi[1];

   if(CopyBuffer(fastHandle, 0, 1, 1, fast) != 1 ||
      CopyBuffer(slowHandle, 0, 1, 1, slow) != 1 ||
      CopyBuffer(atrHandle,  0, 1, 1, atr)  != 1 ||
      CopyBuffer(adxHandle,  0, 1, 1, adx)  != 1 ||
      CopyBuffer(rsiHandle,  0, 1, 1, rsi)  != 1)
   {
      Print("Data indikator belum tersedia: ", GetLastError());
      return;
   }

   double close1 = iClose(_Symbol, PERIOD_H1, 1);

   if(close1 <= 0 || atr[0] <= 0)
      return;

   // Filter out weak trends.
   if(adx[0] < MinADX)
      return;

   bool buySignal =
      fast[0] > slow[0] &&
      close1 > fast[0] &&
      rsi[0] > 50.0 &&
      rsi[0] < 70.0;

   bool sellSignal =
      fast[0] < slow[0] &&
      close1 < fast[0] &&
      rsi[0] < 50.0 &&
      rsi[0] > 30.0;

   if(!buySignal && !sellSignal)
      return;

   bool isBuy = buySignal;
   double stopDistance = atr[0] * SL_ATR;
   double takeDistance = atr[0] * TP_ATR;
   double lot = CalculateLot(stopDistance, isBuy);

   if(lot <= 0)
   {
      Print("Trade dilewati: ukuran lot tidak memenuhi batas risiko.");
      return;
   }

   double ask = SymbolInfoDouble(_Symbol, SYMBOL_ASK);
   double bid = SymbolInfoDouble(_Symbol, SYMBOL_BID);

   bool sent = false;

   if(isBuy)
   {
      double sl = NormalizeDouble(ask - stopDistance, _Digits);
      double tp = NormalizeDouble(ask + takeDistance, _Digits);

      sent = trade.Buy(lot, _Symbol, 0.0, sl, tp,
                       "V3 Trend Buy");
   }
   else
   {
      double sl = NormalizeDouble(bid + stopDistance, _Digits);
      double tp = NormalizeDouble(bid - takeDistance, _Digits);

      sent = trade.Sell(lot, _Symbol, 0.0, sl, tp,
                        "V3 Trend Sell");
   }

   Print("Order=", sent,
         " | retcode=", trade.ResultRetcode(),
         " | ", trade.ResultRetcodeDescription(),
         " | lot=", lot,
         " | ADX=", DoubleToString(adx[0], 2),
         " | RSI=", DoubleToString(rsi[0], 2));

   if(sent && trade.ResultRetcode() == TRADE_RETCODE_DONE)
      lastTradeBar = iTime(_Symbol, PERIOD_H1, 0);
}