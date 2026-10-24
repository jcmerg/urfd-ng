<?php

class xReflector {

   public $Nodes                    = null;
   public $Stations                 = null;
   public $Peers                    = null;
   private $Flagarray               = null;
   private $Flagarray_DXCC          = null;
   private $Flagfile                = null;
   private $ReflectorName           = null;
   private $ServiceUptime           = null;
   private $ProcessIDFile           = null;
   private $XMLContent              = null;
   private $XMLFile                 = null;
   private $ServiceName             = null;
   private $Version                 = null;
   private $DashboardURL            = null;

   public function __construct() {
      $this->Nodes             = array();
      $this->Stations          = array();
      $this->Peers             = array();
   }

   public function LoadXML() {
      if ($this->XMLFile != null) {
         // fread() with the length 0 of an empty file throws on PHP 8
         $this->XMLContent = @file_get_contents($this->XMLFile);
         if (($this->XMLContent === false) || ($this->XMLContent === '')) {
            $this->XMLContent = null;
            return false;
         }

         $this->ServiceName = substr($this->XMLContent, strpos($this->XMLContent, "<XLX")+4, 3);
         if (preg_match('/[^a-zA-Z0-9]/', $this->ServiceName) == 1) {
            $this->ServiceName = null;
            return false;
         }

         $this->ReflectorName = "XLX".$this->ServiceName;

         $LinkedPeersName = "XLX".$this->ServiceName."  linked peers";
         $LinkedNodesName = "XLX".$this->ServiceName."  linked nodes";
         $LinkedUsersName = "XLX".$this->ServiceName."  heard users";

         $XML       = new ParseXML();

         $AllNodesString    = $XML->GetElement($this->XMLContent, $LinkedNodesName);
         $tmpNodes          = $XML->GetAllElements($AllNodesString, "NODE");

         for ($i=0;$i<count($tmpNodes);$i++) {
             $Node = new Node($XML->GetText($tmpNodes[$i], 'Callsign'), $XML->GetText($tmpNodes[$i], 'IP'), $XML->GetText($tmpNodes[$i], 'LinkedModule'), $XML->GetText($tmpNodes[$i], 'Protocol'), $XML->GetText($tmpNodes[$i], 'ConnectTime'), $XML->GetText($tmpNodes[$i], 'LastHeardTime'), CreateCode(16));
             $ts1 = $XML->GetText($tmpNodes[$i], 'LinkedModuleTS1');
             $ts2 = $XML->GetText($tmpNodes[$i], 'LinkedModuleTS2');
             if ($ts1 !== null) $Node->SetLinkedModuleTS1($ts1);
             if ($ts2 !== null) $Node->SetLinkedModuleTS2($ts2);
             $this->AddNode($Node);
         }

         $AllStationsString = $XML->GetElement($this->XMLContent, $LinkedUsersName);
         $tmpStations       = $XML->GetAllElements($AllStationsString, "STATION");
         for ($i=0;$i<count($tmpStations);$i++) {
             $Station = new Station($XML->GetText($tmpStations[$i], 'Callsign'), $XML->GetText($tmpStations[$i], 'Via node'), $XML->GetText($tmpStations[$i], 'Via peer'), $XML->GetText($tmpStations[$i], 'LastHeardTime'), $XML->GetText($tmpStations[$i], 'On module'), $XML->GetText($tmpStations[$i], 'Protocol'), $XML->GetText($tmpStations[$i], 'Locator'), $XML->GetText($tmpStations[$i], 'Latitude'), $XML->GetText($tmpStations[$i], 'Longitude'), $XML->GetText($tmpStations[$i], 'Name'));
             $this->AddStation($Station, false);
         }

         $AllPeersString    = $XML->GetElement($this->XMLContent, $LinkedPeersName);
         $tmpPeers          = $XML->GetAllElements($AllPeersString, "PEER");
         for ($i=0;$i<count($tmpPeers);$i++) {
             $Peer = new Peer($XML->GetText($tmpPeers[$i], 'Callsign'), $XML->GetText($tmpPeers[$i], 'IP'), $XML->GetText($tmpPeers[$i], 'LinkedModule'), $XML->GetText($tmpPeers[$i], 'Protocol'), $XML->GetText($tmpPeers[$i], 'ConnectTime'), $XML->GetText($tmpPeers[$i], 'LastHeardTime'));
             $this->AddPeer($Peer, false);
         }

         $this->Version = $XML->GetText($this->XMLContent, "Version");
         $this->DashboardURL = $XML->GetText($this->XMLContent, "DashboardURL");
      }
   }

   public function GetVersion() {
      return $this->Version;
   }

   // DashboardUrl from urfd.ini, as the reflector reports it
   public function GetDashboardURL() {
      return $this->DashboardURL;
   }

   public function GetXMLContent() {
      return $this->XMLContent;
   }

   public function GetReflectorName() {
      return $this->ReflectorName;
   }

   public function SetXMLFile($XMLFile) {
      if (file_exists($XMLFile) && (is_readable($XMLFile))) {
         $this->XMLFile = $XMLFile;
      }
      else {
         die("File ".$XMLFile." does not exist or is not readable");
         $this->XMLContent = null;
      }
   }

   public function SetPIDFile($ProcessIDFile) {
      if (file_exists($ProcessIDFile)) {
         $this->ProcessIDFile = $ProcessIDFile;
         $this->ServiceUptime = time() - filectime($ProcessIDFile);
      }
      else {
         $this->ProcessIDFile = null;
         $this->ServiceUptime = null;
      }
   }

   public function GetServiceUptime() {
      return $this->ServiceUptime;
   }

   public function SetFlagFile($Flagfile) {
      if (file_exists($Flagfile) && (is_readable($Flagfile))) {
         $this->Flagfile = $Flagfile;
         return true;
      }
      return false;
   }

   public function LoadFlags() {
      if ($this->Flagfile != null) {
         $this->Flagarray = array();
         $this->Flagarray_DXCC = array();
         $handle = fopen($this->Flagfile,"r");
         if ($handle) {
            $i = 0;
            while(!feof($handle)) {
               $row = fgets($handle,1024);
               $tmp = explode(";", $row);

               if (isset($tmp[0])) { $this->Flagarray[$i]['Country'] = $tmp[0]; } else { $this->Flagarray[$i]['Country'] = 'Undefined'; }
               if (isset($tmp[1])) { $this->Flagarray[$i]['ISO']     = $tmp[1]; } else { $this->Flagarray[$i]['ISO'] = "Undefined"; }
               //$this->Flagarray[$i]['DXCC']    = array();
               if (isset($tmp[2])) {
                  $tmp2 = explode("-", $tmp[2]);
                  for ($j=0;$j<count($tmp2);$j++) {
                     //$this->Flagarray[$i]['DXCC'][] = $tmp2[$j];
                     $this->Flagarray_DXCC[ trim($tmp2[$j]) ] = $i;
                  }
               }
               $i++;
            }
            fclose($handle);
         }
         return true;
      }
      return false;
   }

   public function AddNode($NodeObject) {
      if (is_object($NodeObject)) {
         $this->Nodes[] = $NodeObject;
      }
   }

   public function NodeCount() {
      return count($this->Nodes);
   }

   public function GetNode($ArrayIndex) {
      if (isset($this->Nodes[$ArrayIndex])) {
         return $this->Nodes[$ArrayIndex];
      }
      return false;
   }

   public function AddPeer($PeerObject) {
      if (is_object($PeerObject)) {
         $this->Peers[] = $PeerObject;
      }
   }

   public function PeerCount() {
      return count($this->Peers);
   }

   public function GetPeer($ArrayIndex) {
      if (isset($this->Peer[$ArrayIndex])) {
         return $this->Peer[$ArrayIndex];
      }
      return false;
   }

   public function AddStation($StationObject, $AllowDouble = false) {
      if (is_object($StationObject)) {

         if ($AllowDouble) {
            $this->Stations[] = $StationObject;
         }
         else {
            $FoundStationInList = false;
            $i                  = 0;

            $tmp = explode(" ", $StationObject->GetCallsign());
            $RealCallsign       = trim($tmp[0]);

            while (!$FoundStationInList && $i<$this->StationCount()) {
               if ($this->Stations[$i]->GetCallsignOnly() == $RealCallsign) {
                  $FoundStationInList = true;
               }
               $i++;
            }

            if (!$FoundStationInList) {
               if (strlen(trim($RealCallsign)) > 3) {
                  $this->Stations[] = $StationObject;
               }
            }

         }
      }
   }

   public function GetSuffixOfRepeater($Repeater, $LinkedModul, $StartWithIndex = 0) {
      $suffix = "";
      $found  = false;
      $i      = $StartWithIndex;
      while (!$found && $i < $this->NodeCount()) {
         if ($this->Nodes[$i]->GetLinkedModule() == $LinkedModul) {
            if (strpos($this->Nodes[$i]->GetCallSign(), $Repeater) !== false) {
               $suffix = $this->Nodes[$i]->GetSuffix();
               $found = true;
            }
         }
         $i++;
      }
      return $suffix;
   }

   public function GetCallsignAndSuffixByID($RandomId) {
      $suffix   = "";
      $callsign = "";
      $i        = 0;
      while ($i < $this->NodeCount()) {
         if ($this->Nodes[$i]->GetRandomID() == $RandomId) {
            if (trim($this->Nodes[$i]->GetSuffix()) == "") {
               return $this->Nodes[$i]->GetCallSign();
            }
            else {
               return $this->Nodes[$i]->GetCallSign().'-'.$this->Nodes[$i]->GetSuffix();
            }
         }
         $i++;
      }
      return 'N/A';
   }

   public function StationCount() {
      return count($this->Stations);
   }

   public function GetStation($ArrayIndex) {
      if (isset($this->Stations[$ArrayIndex])) {
         return $this->Stations[$ArrayIndex];
      }
      return false;
   }

   public function GetFlag($Callsign) {
      $Image     = "";
      $Letters = 4;
      $Name = "";
      while ($Letters >= 2) {
         $Prefix = substr(trim($Callsign), 0, $Letters);
               if (isset($this->Flagarray_DXCC[$Prefix])) {
                  $Image = $this->Flagarray[ $this->Flagarray_DXCC[$Prefix] ]['ISO'];
                  $Name  = $this->Flagarray[ $this->Flagarray_DXCC[$Prefix] ]['Country'];
                  break;
               }
         $Letters--;
      }
      return array(strtolower($Image), $Name);
   }

   public function GetModules() {
      $out = array();
      for ($i=0;$i<$this->NodeCount();$i++) {
          $Found = false;
          $b = 0;
          while ($b < count($out) && !$Found) {
             if ($out[$b] == $this->Nodes[$i]->GetLinkedModule()) {
                $Found = true;
             }
             $b++;
          }
          if (!$Found && (trim($this->Nodes[$i]->GetLinkedModule()) != "")) {
             $out[] = $this->Nodes[$i]->GetLinkedModule();
          }
      }
      return $out;
   }

   public function GetModuleOfNode($Node) {
      die("FUNCTION DEPRECATED...");
      $Node = trim(str_replace("   ", "-", $Node));
      $Node = trim(str_replace("  ", "-", $Node));
      $Node = trim(str_replace(" ", "-", $Node));
      $found  = false;
      $i      = 0;
      $Module = "";
      while (!$found && $i<$this->NodeCount()) {
         if (strpos($Node, $this->Nodes[$i]->GetFullCallsign()) !== false) {
            $Module = $this->Nodes[$i]->GetLinkedModule();
            $found = true;
         }

         $i++;
      }
      return $Module;
   }

   public function GetCallSignsInModules($Module) {
      $out = array();
      for ($i=0;$i<$this->NodeCount();$i++) {
          if ($this->Nodes[$i]->GetLinkedModule() == $Module) {
             $out[] = $this->Nodes[$i]->GetCallsign();
          }
      }
      return $out;
   }

   public function GetNodesInModulesById($Module) {
      $out = array();
      for ($i=0;$i<$this->NodeCount();$i++) {
          if ($this->Nodes[$i]->GetLinkedModule() == $Module) {
             $out[] = $this->Nodes[$i]->GetRandomID();
          }
      }
      return $out;
   }

}

?>
