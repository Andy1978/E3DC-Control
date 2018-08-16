#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include "RscpProtocol.h"
#include "RscpTags.h"
#include "SocketConnection.h"
#include "AES.h"
#include <time.h>
#include "E3DC_CONF.h"

#define AES_KEY_SIZE        32
#define AES_BLOCK_SIZE      32


static int iSocket = -1;
static int iAuthenticated = 0;
static int iBattPowerStatus = 0; // Status, ob schon mal angefragt,
static int iWBStatus = 0; // Status, WB schon mal angefragt, 0 inaktiv, 1 aktiv, 2 regeln
static int iLMStatus = 0; // Status, Load Management  schon mal angefragt, 0 inaktiv, 1 aktiv, 2 regeln
static float fAvBatterie;
static uint8_t WBchar[8];
static uint8_t WBchar6[6]; // Steuerstring zur Wallbox
const uint16_t iWBLen = 6;

const int cLadeschwelle = 50; // Bis zu dieser Schwelle wird bevorzugt der E3DC-Speicher geladen
const int cLadeende = 90;




static AES aesEncrypter;
static AES aesDecrypter;
static uint8_t ucEncryptionIV[AES_BLOCK_SIZE];
static uint8_t ucDecryptionIV[AES_BLOCK_SIZE];

// static int32_t iPower_Grid;
static uint8_t iCurrent_WB;
static uint8_t iCurrent_Set_WB;
static float fPower_Grid;
static float fAvPower_Grid; // Durchschnitt ungewichtete Netzleistung der letzten 10sec

static float fPower_WB;
static int32_t iPower_PV;
static int32_t iPower_Bat;
static uint8_t iPhases_WB;
static uint8_t iCyc_WB;
static float fBatt_SOC, fBatt_SOC_alt;
static uint32_t iBattLoad;
static int iPowerBalance;

static time_t tE3DC;
static int32_t iFc, iMinLade; // Mindestladeladeleistung des E3DC Speichers
static bool bWBLademodus; // Lademodus der Wallbox; z.B. Sonnenmodus
static bool bWBmaxLadestrom; // Ladestrom der Wallbox per App eingestellt.; 32=ON 31 = OFF
int ControlLoadData(SRscpFrameBuffer * frameBuffer,int32_t Power) {
    RscpProtocol protocol;
    SRscpValue rootValue;
    // The root container is create with the TAG ID 0 which is not used by any device.
    protocol.createContainerValue(&rootValue, 0);
    
    // request Power Meter information
    SRscpValue PMContainer;
//    Power = Power*-1;
    protocol.createContainerValue(&PMContainer, TAG_EMS_REQ_SET_POWER);
    protocol.appendValue(&PMContainer, TAG_EMS_REQ_SET_POWER_MODE,3);
    protocol.appendValue(&PMContainer, TAG_EMS_REQ_SET_POWER_VALUE,Power);
    // append sub-container to root container
    protocol.appendValue(&rootValue, PMContainer);
    // free memory of sub-container as it is now copied to rootValue
    protocol.destroyValueData(PMContainer);
 
    
    
    
    // create buffer frame to send data to the S10
    protocol.createFrameAsBuffer(frameBuffer, rootValue.data, rootValue.length, true); // true to calculate CRC on for transfer
    // the root value object should be destroyed after the data is copied into the frameBuffer and is not needed anymore
    protocol.destroyValueData(rootValue);
    
    return 0;
}
int ControlLoadData2(SRscpFrameBuffer * frameBuffer,int32_t iPower) {
    RscpProtocol protocol;
    SRscpValue rootValue;
    uint32_t uPower;
    if (iPower < 0) uPower = 0; else if (iPower>3000) uPower = 3000; else uPower = iPower;
    // The root container is create with the TAG ID 0 which is not used by any device.
    protocol.createContainerValue(&rootValue, 0);
    
    // request Power Meter information
    SRscpValue PMContainer;
    protocol.createContainerValue(&PMContainer, TAG_EMS_REQ_SET_POWER_SETTINGS);
    protocol.appendValue(&PMContainer, TAG_EMS_POWER_LIMITS_USED,true);
   protocol.appendValue(&PMContainer, TAG_EMS_MAX_CHARGE_POWER,uPower);
//    protocol.appendValue(&PMContainer, TAG_EMS_MAX_DISCHARGE_POWER,300);
//    protocol.appendValue(&PMContainer, TAG_EMS_DISCHARGE_START_POWER,70);
    // append sub-container to root container
    protocol.appendValue(&rootValue, PMContainer);
    // free memory of sub-container as it is now copied to rootValue
    protocol.destroyValueData(PMContainer);
    
    
    
    
    // create buffer frame to send data to the S10
    protocol.createFrameAsBuffer(frameBuffer, rootValue.data, rootValue.length, true); // true to calculate CRC on for transfer
    // the root value object should be destroyed after the data is copied into the frameBuffer and is not needed anymore
    protocol.destroyValueData(rootValue);
    
    return 0;
}

int createRequestWBData(SRscpFrameBuffer * frameBuffer) {
    RscpProtocol protocol;
    SRscpValue rootValue;

    iWBStatus=7;
    
    // The root container is create with the TAG ID 0 which is not used by any device.
    protocol.createContainerValue(&rootValue, 0);
    
    // request Power Meter information
    SRscpValue WBContainer;
    SRscpValue WB2Container;

    // request Wallbox data

    protocol.createContainerValue(&WBContainer, TAG_WB_REQ_DATA) ;
    // add index 0 to select first wallbox
    protocol.appendValue(&WBContainer, TAG_WB_INDEX,0);

    
    protocol.createContainerValue(&WB2Container, TAG_WB_REQ_SET_EXTERN);
    protocol.appendValue(&WB2Container, TAG_WB_EXTERN_DATA_LEN,6);
    protocol.appendValue(&WB2Container, TAG_WB_EXTERN_DATA,WBchar6,iWBLen);

    protocol.appendValue(&WBContainer, WB2Container);
    // free memory of sub-container as it is now copied to rootValue
    protocol.destroyValueData(WB2Container);

    
// append sub-container to root container
    protocol.appendValue(&rootValue, WBContainer);
//    protocol.appendValue(&rootValue, WB2Container);
    // free memory of sub-container as it is now copied to rootValue
    protocol.destroyValueData(WBContainer);
    
    // create buffer frame to send data to the S10
    protocol.createFrameAsBuffer(frameBuffer, rootValue.data, rootValue.length, true); // true to calculate CRC on for transfer
    // the root value object should be destroyed after the data is copied into the frameBuffer and is not needed anymore
    protocol.destroyValueData(rootValue);
    
    return 0;
}




int LoadDataProcess(SRscpFrameBuffer * frameBuffer) {
    const int cLadezeitende1 = 12.5*3600;  // Sommerzeit -2h da GMT = MEZ - 2
    time_t tLadezeitende;  // dynamische Ladezeitberechnung aus dem Cosinus des lfd Tages. 23 Dez = Minimum, 23 Juni = Maximum
    time_t t;
    int32_t tZeitgleichung;
//    float_t test1;
    tm *ts;
    ts = localtime(&tE3DC);
    tLadezeitende = cLadezeitende1+cos((ts->tm_yday+9)*2*3.14/365)*-2*3600;
//    test1 = cos((ts->tm_yday+9)*2*3.14/365);
    t = tE3DC % (24*3600);
//    tLadezeitende = test1;

//  ts = localtime(&t);
    
// Berechnung freie Ladekapazität bis 90% bzw. Ladeende
    
    tZeitgleichung = (-0.171*sin(0.0337 * ts->tm_yday + 0.465) - 0.1299*sin(0.01787 * ts->tm_yday - 0.168))*3600;
    tLadezeitende = tLadezeitende - tZeitgleichung;
    if ((t < (tLadezeitende))&&(fBatt_SOC<cLadeende))
    {
      if ((fBatt_SOC!=fBatt_SOC_alt)||(iFc == 0))
      {
        fBatt_SOC_alt=fBatt_SOC;
        iFc = (cLadeende - fBatt_SOC)*138*3600;
        iFc = iFc / (tLadezeitende-t);
        iMinLade = iFc;
        iFc = (iFc-900)*5;
        if (iFc > 3000) iFc = 3000;
        if (iFc < 500) iFc = 0;
      }
        printf("MinLoad: %u %u ",iMinLade, iFc);
    } else
        if (t > tLadezeitende) iFc = 3000;
           else iFc = 0;
//  Laden auf 100% nach 15:30
    
    printf("GMT %ld:%ld ZG %d ",tLadezeitende/3600,tLadezeitende%3600/60,tZeitgleichung);
    

    
    
    printf("E3DC Zeit: %s", asctime(ts));


    
    const int cEinspeiseLimit = -18000;
    int iPower = 0;
    int maxGrid = -1;
    if (iLMStatus == 0){
        iLMStatus = 5;
        iBattLoad = 3000;
//        ControlLoadData2(frameBuffer,iBattLoad);
    }
    
    if (iLMStatus == 1) {
        
        iLMStatus = 5;
        if (((fBatt_SOC > cLadeschwelle)&&(t<tLadezeitende))||(fBatt_SOC > cLadeende))
  // Überschussleistung=iPower ermitteln
        {
            
        maxGrid = cEinspeiseLimit;
          iPower = (-iPower_Bat + fPower_Grid - maxGrid)*-1;

          if (iPower < 100) {iPower = 0;}
          else
            if (iPower > 3000) {iPower = 3000;}
        
//          if (iPower+200 > fAvBatterie) fAvBatterie = iPower+200; // Überschussladen ohne Überhöhung wg. durchschnittl. Ladeleistung;
            if (iFc > iPower)
            {   iPower = iFc;
                if (iPower > fAvBatterie) iPower = iPower + pow((iPower-fAvBatterie),2)/20;
                if (iPower > 3000) iPower = 3000;
                
            }
            } else
//            if (fBatt_SOC < cLadeende) iPower = 3000;
//            else iPower = 0;
              iPower = 3000;
        
 
        if (abs( int(iPower - iBattLoad)) > 20)
          {
        
            {
            iBattLoad = iPower;
            ControlLoadData2(frameBuffer,iBattLoad);
//            printf("new BattLoad %i\n ",iBattLoad);
            }

          }
    }
    iLMStatus--;
    printf("BattLoad %i\n",iBattLoad);
    
    return 0;
}
int WBProcess(SRscpFrameBuffer * frameBuffer) {
/*   Steuerung der Wallbox
*/
    const int cMinimumladestand = 15;
    const int iMaxcurrent=17;
    static int iDyLadeende;
    

    if (!bWBLademodus) // WB Steuerung nur bei Sonnenmodus
    return 0;

    if (iWBStatus == 0)  {

        iDyLadeende = cLadeschwelle;
        fAvBatterie = 0;
        iFc = 0;
        iBattLoad = 100;
        
        if (fBatt_SOC > iDyLadeende) iDyLadeende = fBatt_SOC;

        
        
        memcpy(WBchar6,"\x01\x06\x00\x00\x00\x00",6);
        WBchar6[1] = WBchar[2];

        if (WBchar[2]==32)
            bWBmaxLadestrom = true; else
            bWBmaxLadestrom = false;

            iWBStatus++;
//         createRequestWBData(frameBuffer);
    }
    fAvBatterie = fAvBatterie*119/120;
    fAvBatterie = fAvBatterie + (float(iPower_Bat)/120);
    
    if (iWBStatus == 1) {
        if (bWBmaxLadestrom)  {//Wenn der Ladestrom auf 32, dann erfolgt keine
            if ((fBatt_SOC>cMinimumladestand)&&(fAvPower_Grid<400)) { //Wenn der Ladestrom auf 32, dann erfolgt keine Begrenzung des Ladestroms im Sonnenmodus
            if ((WBchar6[1]<32)&&(fBatt_SOC>(cMinimumladestand+2))) {
                WBchar6[1]=32;
                createRequestWBData(frameBuffer);
                iWBStatus = 7; }
        }
    }
        
        if (fBatt_SOC > iDyLadeende) iDyLadeende = fBatt_SOC;
        if (fPower_WB == 0) {
            iDyLadeende = cMinimumladestand;
        }
                if ( (fPower_WB == 0) &&
               ( ((fPower_Grid - iPower_Bat)< -5500)
             ||(
                ( ((fPower_Grid - iPower_Bat)< -3300)&&(fBatt_SOC>cMinimumladestand) )
             ||
                ( ((fPower_Grid - iPower_Bat)< -1800)&&(fBatt_SOC>cMinimumladestand)&&(fAvBatterie>iFc) ) // größer Mindesladeschwellex
             ||
                ((fAvPower_Grid< -500)&&(fBatt_SOC>=iDyLadeende))
                )
             )
            && (WBchar6[1] != 6)  // Immer von 6A aus starten
            ) { // Wallbox lädt nicht
            if (not bWBmaxLadestrom)
                { WBchar6[1] = 6;
                createRequestWBData(frameBuffer);
                iWBStatus = 6;
                }
                    else WBchar6[1] = 32;
        }
        if ((fPower_WB > 1000) && not (bWBmaxLadestrom)) { // Wallbox lädt
  
            if (((fPower_Grid< -200)&&(fAvPower_Grid < -100)) && (iPower_Bat >= 0) && (WBchar6[1]<iMaxcurrent)){
                WBchar6[1]++;
                if ((fPower_Grid-iPower_Bat < -5300) && (iPower_Bat >= 0)&& (WBchar6[1]<iMaxcurrent)) WBchar6[1]++;
                if ((fPower_Grid-iPower_Bat < -4500) && (iPower_Bat >= 0)&& (WBchar6[1]<iMaxcurrent)) WBchar6[1]++;
                if ((fPower_Grid-iPower_Bat < -3800) && (iPower_Bat >= 0)&& (WBchar6[1]<iMaxcurrent)) WBchar6[1]++;

                    createRequestWBData(frameBuffer);
                if (WBchar6[1]>16) iWBStatus = 10; else iWBStatus = 6;   // Länger warten bei hohen Stömen

             }
            if (
                 ((iPower_Bat < -2600)&&(WBchar6[1] > 6)) ||
                 ((iPower_Bat < -2000)&&(fAvBatterie<-1000)&&(WBchar6[1] > 6)) ||
                 ((iPower_Bat < 2000) && (iPower_Bat+400 < iBattLoad) &&(fBatt_SOC < cMinimumladestand)&&(WBchar6[1] > 6)) ||
                 ((fBatt_SOC < cLadeende)&&(iPower_Bat<2000)&&(iPower_Bat+800<iBattLoad)&&
                  ((iPower_Bat+400)<iFc)&&((iPower_Bat+400)<iMinLade)&&((fAvBatterie+200)<iFc)&&((fAvBatterie+200)<iMinLade)&&
                  (WBchar6[1]>6))
                 ) { // Mind. 2000W Batterieladen
                WBchar6[1]--;
//                if ((iPower_Bat < -1100)&& (WBchar6[1]>6)) WBchar6[1]--;
                if ((iPower_Bat < -1800)&& (WBchar6[1]>6)) WBchar6[1]--;
                if ((iPower_Bat < -2500)&& (WBchar6[1]>6)) WBchar6[1]--;

                
                if (WBchar6[1]==31) WBchar6[1]--;;
                createRequestWBData(frameBuffer);
                if (WBchar6[1]>16) iWBStatus = 10; else // Länger warten bei hohen Stömen
                iWBStatus = 6;  // Länger warten bei hohen Stömen

            } else
            if (((iPower_Bat < -2700) || ((fPower_Grid > 3000)&&(iPower_Bat<1000)))
                || ((iPower_Bat < -2000)&&(fBatt_SOC < iDyLadeende-1))
                || ((iPower_Bat < -1500)&&(fBatt_SOC < iDyLadeende-2))
                || ((iPower_Bat < -1000)&&(fBatt_SOC < iDyLadeende-3))
                || ((iPower_Bat < -500)&&(fBatt_SOC < iDyLadeende-4))
                || (fAvPower_Grid>400)
                || ((iPower_Bat < -500)&&(fAvBatterie<-600)&&(fBatt_SOC < iDyLadeende-1))
//                || ((iPower_Bat < -1000)&&(fAvBatterie<iFc&&(fAvBatterie<1000)&&(fBatt_SOC < iDyLadeende-1)))
                )  { // höchstens. 1500W Batterieentladen wenn voll
                {if (WBchar6[1] > 5)
                    WBchar6[1]--;
                    createRequestWBData(frameBuffer);
                    if (WBchar6[1] > 6)
                        iWBStatus = 6; else // Warten bis Neustart
                        iWBStatus = 20;  // Warten bis Neustart
                }}
    }
        }
    printf("DyLadeende %i ",iDyLadeende);
    printf("AVBatt   %0.1f iWBStatus %i\n",fAvBatterie,iWBStatus);
    if (iWBStatus > 1) iWBStatus--;
return 0;
}


int createRequestExample(SRscpFrameBuffer * frameBuffer) {
    RscpProtocol protocol;
    SRscpValue rootValue;
    // The root container is create with the TAG ID 0 which is not used by any device.
    protocol.createContainerValue(&rootValue, 0);

    //---------------------------------------------------------------------------------------------------------
    // Create a request frame
    //---------------------------------------------------------------------------------------------------------
    if(iAuthenticated == 0)
    
    {
        printf("\nRequest authentication\n");
        // authentication request
        SRscpValue authenContainer;
        protocol.createContainerValue(&authenContainer, TAG_RSCP_REQ_AUTHENTICATION);
        protocol.appendValue(&authenContainer, TAG_RSCP_AUTHENTICATION_USER, E3DC_USER);
        protocol.appendValue(&authenContainer, TAG_RSCP_AUTHENTICATION_PASSWORD, E3DC_PASSWORD);
        // append sub-container to root container
        protocol.appendValue(&rootValue, authenContainer);
        // free memory of sub-container as it is now copied to rootValue
        protocol.destroyValueData(authenContainer);
    }
    else
    {
        printf("\nRequest cyclic example data\n");
        // request power data information
        protocol.appendValue(&rootValue, TAG_EMS_REQ_POWER_PV);
        protocol.appendValue(&rootValue, TAG_EMS_REQ_POWER_ADD);
        protocol.appendValue(&rootValue, TAG_EMS_REQ_POWER_BAT);
        protocol.appendValue(&rootValue, TAG_EMS_REQ_POWER_HOME);
        protocol.appendValue(&rootValue, TAG_EMS_REQ_POWER_GRID);
        if(iBattPowerStatus == 0)
        {
            protocol.appendValue(&rootValue, TAG_EMS_REQ_GET_POWER_SETTINGS);
            iBattPowerStatus = 1;
        }

        // request battery information
        SRscpValue batteryContainer;
        protocol.createContainerValue(&batteryContainer, TAG_BAT_REQ_DATA);
        protocol.appendValue(&batteryContainer, TAG_BAT_INDEX, (uint8_t)0);
        protocol.appendValue(&batteryContainer, TAG_BAT_REQ_RSOC);
        protocol.appendValue(&batteryContainer, TAG_BAT_REQ_MODULE_VOLTAGE);
        protocol.appendValue(&batteryContainer, TAG_BAT_REQ_CURRENT);
        // append sub-container to root container
        protocol.appendValue(&rootValue, batteryContainer);
        // free memory of sub-container as it is now copied to rootValue
        protocol.destroyValueData(batteryContainer);
        
        // request Power Meter information
        SRscpValue PMContainer;
        protocol.createContainerValue(&PMContainer, TAG_PM_REQ_DATA);
        protocol.appendValue(&PMContainer, TAG_PM_INDEX, (uint8_t)6);
        protocol.appendValue(&PMContainer, TAG_PM_REQ_POWER_L1);
        protocol.appendValue(&PMContainer, TAG_PM_REQ_POWER_L2);
        protocol.appendValue(&PMContainer, TAG_PM_REQ_POWER_L3);
        protocol.appendValue(&PMContainer, TAG_PM_REQ_VOLTAGE_L1);
        protocol.appendValue(&PMContainer, TAG_PM_REQ_VOLTAGE_L2);
        protocol.appendValue(&PMContainer, TAG_PM_REQ_VOLTAGE_L3);
        // append sub-container to root container
        protocol.appendValue(&rootValue, PMContainer);
        // free memory of sub-container as it is now copied to rootValue
        protocol.destroyValueData(PMContainer);

        // request Wallbox information
        SRscpValue WBContainer;
  

         protocol.createContainerValue(&WBContainer, TAG_WB_REQ_AVAILABLE_SOLAR_POWER);
         protocol.appendValue(&WBContainer, TAG_WB_INDEX, (uint8_t)0);

        // append sub-container to root container
         protocol.appendValue(&rootValue, WBContainer);
         // free memory of sub-container as it is now copied to rootValue
         protocol.destroyValueData(WBContainer);
 
        
        protocol.createContainerValue(&WBContainer, TAG_WB_REQ_DATA);
        protocol.appendValue(&WBContainer, TAG_WB_INDEX, (uint8_t)0);
//        protocol.appendValue(&WBContainer, TAG_WB_REQ_PM_MODE);
        protocol.appendValue(&WBContainer, TAG_WB_REQ_PARAM_1);

        protocol.appendValue(&WBContainer, TAG_WB_REQ_PM_POWER_L1);
        protocol.appendValue(&WBContainer, TAG_WB_REQ_PM_POWER_L2);
        protocol.appendValue(&WBContainer, TAG_WB_REQ_PM_POWER_L3);
//        protocol.appendValue(&WBContainer, TAG_WB_REQ_AVAILABLE_SOLAR_POWER);

        // append sub-container to root container
        protocol.appendValue(&rootValue, WBContainer);
        // free memory of sub-container as it is now copied to rootValue
        protocol.destroyValueData(WBContainer);

        
        if(iBattPowerStatus == 2)
            
        {
            // request RootPower Meter information
        }

    }

    
    // create buffer frame to send data to the S10
    protocol.createFrameAsBuffer(frameBuffer, rootValue.data, rootValue.length, true); // true to calculate CRC on for transfer
    // the root value object should be destroyed after the data is copied into the frameBuffer and is not needed anymore
    protocol.destroyValueData(rootValue);
    printf("\nRequest cyclic example data done\n");

    return 0;
}

int handleResponseValue(RscpProtocol *protocol, SRscpValue *response)
{

    // check if any of the response has the error flag set and react accordingly
    if(response->dataType == RSCP::eTypeError) {
        // handle error for example access denied errors
        uint32_t uiErrorCode = protocol->getValueAsUInt32(response);
        printf("Tag 0x%08X received error code %u.\n", response->tag, uiErrorCode);
        return -1;
    }

    

    // check the SRscpValue TAG to detect which response it is
    switch(response->tag){
    case TAG_RSCP_AUTHENTICATION: {
        // It is possible to check the response->dataType value to detect correct data type
        // and call the correct function. If data type is known,
        // the correct function can be called directly like in this case.
        uint8_t ucAccessLevel = protocol->getValueAsUChar8(response);
        if(ucAccessLevel > 0) {
            iAuthenticated = 1;
        }
        printf("RSCP authentitication level %i\n", ucAccessLevel);
        break;
    }
    case TAG_EMS_POWER_PV: {    // response for TAG_EMS_REQ_POWER_PV
        int32_t iPower = protocol->getValueAsInt32(response);
        printf("EMS PV %i", iPower);
        iPower_PV = iPower;
        break;
    }
    case TAG_EMS_POWER_BAT: {    // response for TAG_EMS_REQ_POWER_BAT
        iPower_Bat = protocol->getValueAsInt32(response);
        printf(" BAT %i", iPower_Bat);
        break;
    }
    case TAG_EMS_POWER_HOME: {    // response for TAG_EMS_REQ_POWER_HOME
        int32_t iPower2 = protocol->getValueAsInt32(response);
        printf(" home %i", iPower2);
        iPowerBalance = iPower2;

        break;
    }
    case TAG_EMS_POWER_GRID: {    // response for TAG_EMS_REQ_POWER_GRID
        int32_t iPower = protocol->getValueAsInt32(response);
        iPowerBalance = iPowerBalance- iPower_PV + iPower_Bat - iPower;
        printf(" grid %i", iPower);
        printf(" E3DC %i ", -iPowerBalance - int(fPower_WB));
        printf(" # %i\n", iPower_PV - iPower_Bat + iPower - int(fPower_WB));
        break;
    }
    case TAG_EMS_POWER_ADD: {    // response for TAG_EMS_REQ_POWER_ADD
        int32_t iPower = protocol->getValueAsInt32(response);

        printf(" add %i", - iPower);
        iPower_PV = iPower_PV - iPower;
        printf(" # %i", iPower_PV);
        break;
    }
    case TAG_EMS_SET_POWER: {    // response for TAG_EMS_REQ_POWER_ADD
            int32_t iPower = protocol->getValueAsInt32(response);
            
            printf(" SET %i\n", iPower);
            break;
        }
    case TAG_PM_POWER_L1: {    // response for TAG_EMS_REQ_POWER_ADD
            int32_t iPower = protocol->getValueAsInt32(response);
            printf("L1 is %i W\n", iPower);
            break;
    }
   case TAG_BAT_DATA: {        // resposne for TAG_BAT_REQ_DATA
        uint8_t ucBatteryIndex = 0;
        std::vector<SRscpValue> batteryData = protocol->getValueAsContainer(response);
        for(size_t i = 0; i < batteryData.size(); ++i) {
            if(batteryData[i].dataType == RSCP::eTypeError) {
                // handle error for example access denied errors
                uint32_t uiErrorCode = protocol->getValueAsUInt32(&batteryData[i]);
                printf("Tag 0x%08X received error code %u.\n", batteryData[i].tag, uiErrorCode);
                return -1;
            }
            // check each battery sub tag
            switch(batteryData[i].tag) {
            case TAG_BAT_INDEX: {
                ucBatteryIndex = protocol->getValueAsUChar8(&batteryData[i]);
                break;
            }
            case TAG_BAT_RSOC: {              // response for TAG_BAT_REQ_RSOC
                fBatt_SOC = protocol->getValueAsFloat32(&batteryData[i]);
                printf("Battery SOC %0.1f %% ", fBatt_SOC);
                break;
            }
            case TAG_BAT_MODULE_VOLTAGE: {    // response for TAG_BAT_REQ_MODULE_VOLTAGE
                float fVoltage = protocol->getValueAsFloat32(&batteryData[i]);
                printf(" %0.1f V ", fVoltage);
                break;
            }
            case TAG_BAT_CURRENT: {    // response for TAG_BAT_REQ_CURRENT
                float fVoltage = protocol->getValueAsFloat32(&batteryData[i]);
                printf(" %0.1f A\n", fVoltage);
                break;
            }
            case TAG_BAT_STATUS_CODE: {    // response for TAG_BAT_REQ_STATUS_CODE
                uint32_t uiErrorCode = protocol->getValueAsUInt32(&batteryData[i]);
                printf("Battery status code is 0x%08X\n", uiErrorCode);
                break;
            }
            case TAG_BAT_ERROR_CODE: {    // response for TAG_BAT_REQ_ERROR_CODE
                uint32_t uiErrorCode = protocol->getValueAsUInt32(&batteryData[i]);
                printf("Battery error code is 0x%08X\n", uiErrorCode);
                break;
            }
            // ...
            default:
                // default behaviour
                printf("Unknown battery tag %08X\n", response->tag);
                break;
            }
        }
        protocol->destroyValueData(batteryData);
        break;
    }
        case TAG_PM_DATA: {        // resposne for TAG_PM_REQ_DATA
            uint8_t ucPMIndex = 0;
            std::vector<SRscpValue> PMData = protocol->getValueAsContainer(response);
            for(size_t i = 0; i < PMData.size(); ++i) {
                if(PMData[i].dataType == RSCP::eTypeError) {
                    // handle error for example access denied errors
                    uint32_t uiErrorCode = protocol->getValueAsUInt32(&PMData[i]);
                    printf("Tag 0x%08X received error code %u.\n", PMData[i].tag, uiErrorCode);
                    return -1;
                }
                // check each PM sub tag
                switch(PMData[i].tag) {
                    case TAG_PM_INDEX: {
                        ucPMIndex = protocol->getValueAsUChar8(&PMData[i]);
                        break;
                    }
                    case TAG_PM_POWER_L1: {              // response for TAG_PM_REQ_L1
                        float fPower = protocol->getValueAsDouble64(&PMData[i]);
                        printf("Grid is %0.1f W", fPower);
                        fPower_Grid = fPower;
                        break;
                    }
                    case TAG_PM_POWER_L2: {              // response for TAG_PM_REQ_L2
                        float fPower = protocol->getValueAsDouble64(&PMData[i]);
                        printf(" %0.1f W", fPower);
                        fPower_Grid = fPower_Grid + fPower;
                        break;
                    }
                    case TAG_PM_POWER_L3: {              // response for TAG_PM_REQ_L3
                        float fPower = protocol->getValueAsDouble64(&PMData[i]);
                        printf(" %0.1f W ", fPower);
                        fPower_Grid = fPower_Grid + fPower;
                        fAvPower_Grid = fAvPower_Grid*19/20 + fPower_Grid/20;
                        printf(" # %0.1f W", fPower_Grid);
                        printf(" & %0.01f W\n", fAvPower_Grid);
                        break;
                    }
                    case TAG_PM_VOLTAGE_L1: {              // response for TAG_PM_REQ_L1
                        float fPower = protocol->getValueAsFloat32(&PMData[i]);
                        printf(" %0.1f V", fPower);
                        break;
                    }
                    case TAG_PM_VOLTAGE_L2: {              // response for TAG_PM_REQ_L2
                        float fPower = protocol->getValueAsFloat32(&PMData[i]);
                        printf(" %0.1f V", fPower);
                        break;
                    }
                    case TAG_PM_VOLTAGE_L3: {              // response for TAG_PM_REQ_L3
                        float fPower = protocol->getValueAsFloat32(&PMData[i]);
                        printf(" %0.1f V\n", fPower);
                        break;
                    }
                        // ...
                    default:
                        // default behaviour
                        printf("Unknown Grid tag %08X\n", response->tag);
                        printf("Unknown Grid datatype %08X\n", response->dataType);
                        break;
                }
            }
            protocol->destroyValueData(PMData);
            break;
        }

        case TAG_WB_AVAILABLE_SOLAR_POWER: {              // response for TAG_WB_AVAILABLE_SOLAR_POWER
            uint8_t ucPMIndex = 0;
            std::vector<SRscpValue> PMData = protocol->getValueAsContainer(response);
            for(size_t i = 0; i < PMData.size(); ++i) {
                if(PMData[i].dataType == RSCP::eTypeError) {
                    // handle error for example access denied errors
                    uint32_t uiErrorCode = protocol->getValueAsUInt32(&PMData[i]);
                    printf("Tag 0x%08X received error code %u.\n", PMData[i].tag, uiErrorCode);
                    return -1;
                }
                printf(" datatype %08X", PMData[i].dataType);
                printf(" length %02X", PMData[i].length);
                printf(" data");
                for (size_t y = 0; y<PMData[i].length; y++) {
                    if (y%4 == 0)
                        printf(" ");
                    printf("%02X", PMData[i].data[y]);
                }
                printf("\n");

/*
                
            float fPower = protocol->getValueAsDouble64(&PMData[i]);
             printf(" %0.1f W", fPower);
             fPower_WB = fPower_WB + fPower;
             printf(" Solar %0.1f W\n", fPower_WB);
*/             break;
            }}

//        case TAG_WB_AVAILABLE_SOLAR_POWER:              // response for TAG_WB_AVAILABLE_SOLAR_POWER

        case TAG_WB_DATA: {        // resposne for TAG_WB_DATA
            uint8_t ucPMIndex = 0;
            std::vector<SRscpValue> PMData = protocol->getValueAsContainer(response);
            for(size_t i = 0; i < PMData.size(); ++i) {
                if(PMData[i].dataType == RSCP::eTypeError) {
                    // handle error for example access denied errors
                    uint32_t uiErrorCode = protocol->getValueAsUInt32(&PMData[i]);
                    printf("Tag 0x%08X received error code %u.\n", PMData[i].tag, uiErrorCode);
                    return -1;
                }
                // check each PM sub tag
                switch(PMData[i].tag) {
                    case TAG_WB_INDEX: {
                        ucPMIndex = protocol->getValueAsUChar8(&PMData[i]);
                        break;
                    }
                    case TAG_WB_PM_POWER_L1: {              // response for TAG_PM_REQ_L1
                        float fPower = protocol->getValueAsDouble64(&PMData[i]);
                        printf("WB is %0.1f W", fPower);
                        fPower_WB = fPower;
/*                        if (iCyc_WB>0) iCyc_WB--;
                        if (fPower > 1)
                        {   if (fPower_Grid < -800)
                            if ((WBchar[2] < 16) && (iCyc_WB == 0))
                            {WBchar[2]++;
                            iWBStatus = 2;
                            iCyc_WB = 3;
                            }
                        if (iPower_Bat < -300)
                        if (WBchar[2] > 6)
                        {   WBchar[2]--;
                            iWBStatus = 2;
                            iCyc_WB = 3;
                        }}
*/
                        break;
                    }
                    case TAG_WB_PM_POWER_L2: {              // response for TAG_PM_REQ_L2
                        float fPower = protocol->getValueAsDouble64(&PMData[i]);
                        printf(" %0.1f W", fPower);
                        fPower_WB = fPower_WB + fPower;
                        break;
                    }
                    case TAG_WB_PM_POWER_L3: {              // response for TAG_PM_REQ_L3
                        float fPower = protocol->getValueAsDouble64(&PMData[i]);
                        printf(" %0.1f W", fPower);
                        fPower_WB = fPower_WB + fPower;
                        printf(" Total %0.1f W", fPower_WB);
                        break;
                    }
/*                    case TAG_WB_AVAILABLE_SOLAR_POWER: {              // response for TAG_PM_REQ_L3
                        float fPower = protocol->getValueAsDouble64(&PMData[i]);
                        printf(" %0.1f W", fPower);
                        fPower_WB = fPower_WB + fPower;
                        printf(" Solar %0.1f W\n", fPower_WB);
                        break;
                    }
*/
 /*                    case TAG_WB_EXTERN_DATA: {              // response for TAG_RSP_PARAM_1
                        printf(" WB EXTERN_DATA\n");
                    }
                    case TAG_WB_EXTERN_DATA_LEN: {              // response for TAG_RSP_PARAM_1
                        printf(" WB EXTERN_DATA_LEN\n");
 
                    }
*/
                    case (TAG_WB_RSP_PARAM_1): {              // response for TAG_RSP_PARAM_1

                        
/*                        printf(" WB Param_1\n");
                        printf(" datatype %08X", PMData[i].dataType);
                        printf(" length %02X", PMData[i].length);
                        printf(" data");
                        for (size_t y = 0; y<PMData[i].length; y++) {
                            if (y%4 == 0)
                            printf(" ");
                        printf("%02X", PMData[i].data[y]);
                        }
                        printf("\n");
*/
                        std::vector<SRscpValue> WBData = protocol->getValueAsContainer(&PMData[i]);

                        for(size_t i = 0; i < WBData.size(); ++i) {
                            if(WBData[i].dataType == RSCP::eTypeError) {
                                // handle error for example access denied errors
                                uint32_t uiErrorCode = protocol->getValueAsUInt32(&WBData[i]);
                                printf("Tag 0x%08X received error code %u.\n", WBData[i].tag, uiErrorCode);
                                return -1;
                            }
                            // check each PM sub tag
                            switch(WBData[i].tag) {
                                case TAG_WB_EXTERN_DATA: {              // response for TAG_RSP_PARAM_1
//                                    printf(" WB EXTERN_DATA\n");
                                    memcpy(&WBchar,&WBData[i].data[0],sizeof(WBchar));
                                    bWBLademodus = (WBchar[0]&1);
                                    if (bWBLademodus) printf("Sonnenmodus:");
                                    printf(" MODUS ist %u",WBchar[0]);
                                    printf(" Ladestromstärke ist %uA\n",WBchar[2]);
                                    if (WBchar[2]==32) {
                                        bWBmaxLadestrom=true;
                                    }
                                    if (WBchar[2]==31) {
                                        bWBmaxLadestrom=false;
                                    }
                                    break;
                                }
                                case TAG_WB_EXTERN_DATA_LEN: {              // response for TAG_RSP_PARAM_1
                                    uint8_t iLen = protocol->getValueAsUChar8(&WBData[i]);

//                                    printf(" WB EXTERN_DATA_LEN %u\n",iLen);
                                    break;
                                    
                                }
                                default:

                                    printf("Unknown WB tag %08X", WBData[i].tag);
                                    printf(" datatype %08X", WBData[i].dataType);
                            }
/*                                    printf(" length %02X", WBData[i].length);
                                    printf(" data %02X", WBData[i].data[0]);
                                    printf("%02X", WBData[i].data[1]);
                                    printf("%02X", WBData[i].data[2]);
                                    printf("%02X\n", WBData[i].data[3]);
*/                        }
                       
                            protocol->destroyValueData(WBData);
                        break;

                    }
                    // ...
                    default:
                        // default behaviour
                        printf("Unknown WB tag %08X", PMData[i].tag);
                        printf(" datatype %08X", PMData[i].dataType);
                        printf(" length %02X", PMData[i].length);
                        printf(" data %02X", PMData[i].data[0]);
                        printf("%02X", PMData[i].data[1]);
                        printf("%02X", PMData[i].data[2]);
                        printf("%02X\n", PMData[i].data[3]);
                        sleep(1);
                        break;
                }
            }
            protocol->destroyValueData(PMData);
            break;
        }
        case TAG_EMS_GET_POWER_SETTINGS:         // resposne for TAG_PM_REQ_DATA
        case TAG_EMS_SET_POWER_SETTINGS: {        // resposne for TAG_PM_REQ_DATA
            uint8_t ucPMIndex = 0;
            std::vector<SRscpValue> PMData = protocol->getValueAsContainer(response);
            for(size_t i = 0; i < PMData.size(); ++i) {
                if(PMData[i].dataType == RSCP::eTypeError) {
                    // handle error for example access denied errors
                    uint32_t uiErrorCode = protocol->getValueAsUInt32(&PMData[i]);
                    printf("TAG_EMS_GET_POWER_SETTINGS 0x%08X received error code %u.\n", PMData[i].tag, uiErrorCode);
                    return -1;
                }
                // check each PM sub tag
                switch(PMData[i].tag) {
                    case TAG_PM_INDEX: {
                        ucPMIndex = protocol->getValueAsUChar8(&PMData[i]);
                        break;
                    }
                    case TAG_EMS_POWER_LIMITS_USED: {              // response for POWER_LIMITS_USED
                        if (protocol->getValueAsBool(&PMData[i])){
                            printf("POWER_LIMITS_USED\n");
                            }
                        break;
                    }
                    case TAG_EMS_MAX_CHARGE_POWER: {              // 101 response for TAG_EMS_MAX_CHARGE_POWER
                        uint32_t uPower = protocol->getValueAsUInt32(&PMData[i]);
                        printf("MAX_CHARGE_POWER %i W\n", uPower);
                        break;
                    }
                    case TAG_EMS_MAX_DISCHARGE_POWER: {              //102 response for TAG_EMS_MAX_DISCHARGE_POWER
                        uint32_t uPower = protocol->getValueAsUInt32(&PMData[i]);
                        printf("MAX_DISCHARGE_POWER %i W\n", uPower);
                        break;
                    }
                    case TAG_EMS_DISCHARGE_START_POWER:{              //103 response for TAG_EMS_DISCHARGE_START_POWER
                        uint32_t uPower = protocol->getValueAsUInt32(&PMData[i]);
                        printf("DISCHARGE_START_POWER %i W\n", uPower);
                        break;
                    }
                    case TAG_EMS_POWERSAVE_ENABLED: {              //104 response for TAG_EMS_POWERSAVE_ENABLED
                        if (protocol->getValueAsBool(&PMData[i])){
                            printf("POWERSAVE_ENABLED\n");
                        }
                        break;
                    }
                    case TAG_EMS_WEATHER_REGULATED_CHARGE_ENABLED: {//105 resp WEATHER_REGULATED_CHARGE_ENABLED
                        if (protocol->getValueAsBool(&PMData[i])){
                            printf("WEATHER_REGULATED_CHARGE_ENABLED\n");
                        }
                        break;
                    }
                        // ...
                    default:
                        // default behaviour
/*                        printf("Unkonwn GET_POWER_SETTINGS tag %08X", PMData[i].tag);
                        printf(" len %08X", PMData[i].length);
                        printf(" datatype %08X\n", PMData[i].dataType);
                        uint32_t uPower = protocol->getValueAsUInt32(&PMData[i]);
                        printf(" Value  %i\n", uPower);
*/                        break;
                }
            }
            protocol->destroyValueData(PMData);
//            sleep(10);
            break;
        }
    // ...
    default:
        // default behavior
        printf("Unknown tag %08X\n", response->tag);
        break;
    }
    return 0;

}

static int processReceiveBuffer(const unsigned char * ucBuffer, int iLength)
{
    RscpProtocol protocol;
    SRscpFrame frame;

    int iResult = protocol.parseFrame(ucBuffer, iLength, &frame);
    if(iResult < 0) {
        // check if frame length error occured
        // in that case the full frame length was not received yet
        // and the receive function must get more data
        if(iResult == RSCP::ERR_INVALID_FRAME_LENGTH) {
            return 0;
        }
        // otherwise a not recoverable error occured and the connection can be closed
        else {
            return iResult;
        }
    }

    int iProcessedBytes = iResult;

// Auslesen Zeitstempel aus dem Frame von der E3DC und ausgeben
//    time_t t;
//    struct tm * ts;
    
      tE3DC = frame.header.timestamp.seconds;
//    ts = localtime(&tE3DC);
    
//    printf("E3DC Zeit: %s", asctime(ts));

    

    // process each SRscpValue struct seperately
    for(unsigned int i=0; i < frame.data.size(); i++) {
        handleResponseValue(&protocol, &frame.data[i]);
    }

    // destroy frame data and free memory
    protocol.destroyFrameData(frame);

    // returned processed amount of bytes
    return iProcessedBytes;
}

static void receiveLoop(bool & bStopExecution)
{
    //--------------------------------------------------------------------------------------------------------------
    // RSCP Receive Frame Block Data
    //--------------------------------------------------------------------------------------------------------------
    // setup a static dynamic buffer which is dynamically expanded (re-allocated) on demand
    // the data inside this buffer is not released when this function is left
    static int iReceivedBytes = 0;
    static std::vector<uint8_t> vecDynamicBuffer;

    // check how many RSCP frames are received, must be at least 1
    // multiple frames can only occur in this example if one or more frames are received with a big time delay
    // this should usually not occur but handling this is shown in this example
    int iReceivedRscpFrames = 0;
    while(!bStopExecution && ((iReceivedBytes > 0) || iReceivedRscpFrames == 0))
    {
        // check and expand buffer
        if((vecDynamicBuffer.size() - iReceivedBytes) < 4096) {
            // check maximum size
            if(vecDynamicBuffer.size() > RSCP_MAX_FRAME_LENGTH) {
                // something went wrong and the size is more than possible by the RSCP protocol
                printf("Maximum buffer size exceeded %i\n", vecDynamicBuffer.size());
                bStopExecution = true;
                break;
            }
            // increase buffer size by 4096 bytes each time the remaining size is smaller than 4096
            vecDynamicBuffer.resize(vecDynamicBuffer.size() + 4096);
        }
        // receive data
        int iResult = SocketRecvData(iSocket, &vecDynamicBuffer[0] + iReceivedBytes, vecDynamicBuffer.size() - iReceivedBytes);
        if(iResult < 0)
        {
            // check errno for the error code to detect if this is a timeout or a socket error
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                // receive timed out -> continue with re-sending the initial block
                printf("Response receive timeout (retry)\n");
                break;
            }
            // socket error -> check errno for failure code if needed
            printf("Socket receive error. errno %i\n", errno);
            bStopExecution = true;
            break;
        }
        else if(iResult == 0)
        {
            // connection was closed regularly by peer
            // if this happens on startup each time the possible reason is
            // wrong AES password or wrong network subnet (adapt hosts.allow file required)
            printf("Connection closed by peer\n");
            bStopExecution = true;
            break;
        }
        // increment amount of received bytes
        iReceivedBytes += iResult;

        // process all received frames
        while (!bStopExecution)
        {
            // round down to a multiple of AES_BLOCK_SIZE
            int iLength = ROUNDDOWN(iReceivedBytes, AES_BLOCK_SIZE);
            // if not even 32 bytes were received then the frame is still incomplete
            if(iLength == 0) {
                break;
            }
            // resize temporary decryption buffer
            std::vector<uint8_t> decryptionBuffer;
            decryptionBuffer.resize(iLength);
            // initialize encryption sequence IV value with value of previous block
            aesDecrypter.SetIV(ucDecryptionIV, AES_BLOCK_SIZE);
            // decrypt data from vecDynamicBuffer to temporary decryptionBuffer
            aesDecrypter.Decrypt(&vecDynamicBuffer[0], &decryptionBuffer[0], iLength / AES_BLOCK_SIZE);

            // data was received, check if we received all data
            int iProcessedBytes = processReceiveBuffer(&decryptionBuffer[0], iLength);
            if(iProcessedBytes < 0) {
                // an error occured;
                printf("Error parsing RSCP frame: %i\n", iProcessedBytes);
                // stop execution as the data received is not RSCP data
                bStopExecution = true;
                break;

            }
            else if(iProcessedBytes > 0) {
                // round up the processed bytes as iProcessedBytes does not include the zero padding bytes
                iProcessedBytes = ROUNDUP(iProcessedBytes, AES_BLOCK_SIZE);
                // store the IV value from encrypted buffer for next block decryption
                memcpy(ucDecryptionIV, &vecDynamicBuffer[0] + iProcessedBytes - AES_BLOCK_SIZE, AES_BLOCK_SIZE);
                // move the encrypted data behind the current frame data (if any received) to the front
                memcpy(&vecDynamicBuffer[0], &vecDynamicBuffer[0] + iProcessedBytes, vecDynamicBuffer.size() - iProcessedBytes);
                // decrement the total received bytes by the amount of processed bytes
                iReceivedBytes -= iProcessedBytes;
                // increment a counter that a valid frame was received and
                // continue parsing process in case a 2nd valid frame is in the buffer as well
                iReceivedRscpFrames++;
            }
            else {
                // iProcessedBytes is 0
                // not enough data of the next frame received, go back to receive mode if iReceivedRscpFrames == 0
                // or transmit mode if iReceivedRscpFrames > 0
                break;
            }
        }
    }
}

static void mainLoop(void)
{
    RscpProtocol protocol;
    bool bStopExecution = false;

    while(!bStopExecution)
    {
        
        //--------------------------------------------------------------------------------------------------------------
        // RSCP Transmit Frame Block Data
        //--------------------------------------------------------------------------------------------------------------
        SRscpFrameBuffer frameBuffer;
        memset(&frameBuffer, 0, sizeof(frameBuffer));

        // create an RSCP frame with requests to some example data
        if(iAuthenticated == 1) {
            WBProcess(&frameBuffer);
        if(frameBuffer.dataLength == 0)
            LoadDataProcess(&frameBuffer);
        }
        // check that frame data was created
        
        if(frameBuffer.dataLength == 0){
            sleep(1);
            createRequestExample(&frameBuffer);
        }
        // check that frame data was created

        if(frameBuffer.dataLength > 0)
        {
            // resize temporary encryption buffer to a multiple of AES_BLOCK_SIZE
            std::vector<uint8_t> encryptionBuffer;
            encryptionBuffer.resize(ROUNDUP(frameBuffer.dataLength, AES_BLOCK_SIZE));
            // zero padding for data above the desired length
            memset(&encryptionBuffer[0] + frameBuffer.dataLength, 0, encryptionBuffer.size() - frameBuffer.dataLength);
            // copy desired data length
            memcpy(&encryptionBuffer[0], frameBuffer.data, frameBuffer.dataLength);
            // set continues encryption IV
            aesEncrypter.SetIV(ucEncryptionIV, AES_BLOCK_SIZE);
            // start encryption from encryptionBuffer to encryptionBuffer, blocks = encryptionBuffer.size() / AES_BLOCK_SIZE
            aesEncrypter.Encrypt(&encryptionBuffer[0], &encryptionBuffer[0], encryptionBuffer.size() / AES_BLOCK_SIZE);
            // save new IV for next encryption block
            memcpy(ucEncryptionIV, &encryptionBuffer[0] + encryptionBuffer.size() - AES_BLOCK_SIZE, AES_BLOCK_SIZE);

            // send data on socket
            int iResult = SocketSendData(iSocket, &encryptionBuffer[0], encryptionBuffer.size());
            if(iResult < 0) {
                printf("Socket send error %i. errno %i\n", iResult, errno);
                bStopExecution = true;
            }
            else {
                // go into receive loop and wait for response
                receiveLoop(bStopExecution);
            }
        }
        // free frame buffer memory
        protocol.destroyFrameData(&frameBuffer);

        // main loop sleep / cycle time before next request
    }
}

int main(int argc, char *argv[])
{
    // endless application which re-connections to server on connection lost
    while(true)
    {
        // connect to server
        printf("Connecting to server %s:%i\n", SERVER_IP, SERVER_PORT);
        iSocket = SocketConnect(SERVER_IP, SERVER_PORT);
        if(iSocket < 0) {
            printf("Connection failed\n");
            sleep(1);
            continue;
        }
        printf("Connected successfully\n");

        // reset authentication flag
        iAuthenticated = 0;

        // create AES key and set AES parameters
        {
            // initialize AES encryptor and decryptor IV
            memset(ucDecryptionIV, 0xff, AES_BLOCK_SIZE);
            memset(ucEncryptionIV, 0xff, AES_BLOCK_SIZE);

            // limit password length to AES_KEY_SIZE
            int iPasswordLength = strlen(AES_PASSWORD);
            if(iPasswordLength > AES_KEY_SIZE)
                iPasswordLength = AES_KEY_SIZE;

            // copy up to 32 bytes of AES key password
            uint8_t ucAesKey[AES_KEY_SIZE];
            memset(ucAesKey, 0xff, AES_KEY_SIZE);
            memcpy(ucAesKey, AES_PASSWORD, iPasswordLength);

            // set encryptor and decryptor parameters
            aesDecrypter.SetParameters(AES_KEY_SIZE * 8, AES_BLOCK_SIZE * 8);
            aesEncrypter.SetParameters(AES_KEY_SIZE * 8, AES_BLOCK_SIZE * 8);
            aesDecrypter.StartDecryption(ucAesKey);
            aesEncrypter.StartEncryption(ucAesKey);
        }

        // enter the main transmit / receive loop
        mainLoop();
        printf("mainloop beendet");
        // close socket connection
        SocketClose(iSocket);
        iSocket = -1;
        sleep(10);
    }
    return 0;
}
