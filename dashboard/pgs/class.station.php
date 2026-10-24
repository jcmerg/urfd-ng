<?php

class Station {
   
   private $Callsign;
   private $Via;
   private $LastHeardTime;
   private $Suffix;
   private $CallsignOnly;
   private $Peer;
   private $OnModule;
   private $Protocol;
   private $Locator;
   private $Latitude;
   private $Longitude;
   private $Name;

   public function __construct($Callsign, $Via, $Peer, $LastHeardTime, $OnModule, $Protocol = '', $Locator = '', $Latitude = '', $Longitude = '', $Name = '') {
      $this->Callsign      = trim($Callsign);
      $this->Via           = trim($Via);
      $this->Peer          = trim($Peer);
      $this->LastHeardTime = ParseTime($LastHeardTime);
      if (strpos($Callsign, " / ") !== false) {
         $this->Suffix = trim(substr($Callsign, strpos($Callsign, " / ")+3, strlen($Callsign)));
      }
      else {
         $this->Suffix = "";
      }
      
      $tmp = explode(" ", $Callsign);
      $this->CallsignOnly = $tmp[0];
      $this->OnModule     = $OnModule;
      $this->Protocol     = trim($Protocol);
      // position reported by M17 GNSS META, empty if none
      $this->Locator      = trim($Locator);
      $this->Latitude     = trim($Latitude);
      $this->Longitude    = trim($Longitude);
      // operator's first name from the DMR or NXDN ID database, empty if unknown
      $this->Name         = trim((string)$Name);
   }
 
   public function GetCallsign()             { return $this->Callsign;       }
   public function GetVIA()                  { return $this->Via;            }
   public function GetPeer()                 { return $this->Peer;           }
   public function GetLastHeardTime()        { return $this->LastHeardTime;  }
   public function GetSuffix()               { return $this->Suffix;         }
   public function GetCallsignOnly()         { return $this->CallsignOnly;   }
   public function GetModule()               { return $this->OnModule;       }
   public function GetProtocol()             { return $this->Protocol;       }
   public function GetLocator()              { return $this->Locator;        }
   public function GetLatitude()             { return $this->Latitude;       }
   public function GetLongitude()            { return $this->Longitude;      }
   public function GetName()                 { return $this->Name;           }
   
}

?>