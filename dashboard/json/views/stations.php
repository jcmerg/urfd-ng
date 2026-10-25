<?php

/*** Add stations to payload ***/
// Initialise so no recent traffic encodes as {"stations":[]} rather than null.
$payload['stations'] = array();

for ($i=0;$i<$Reflector->StationCount();$i++) {

    $tmp = preg_split('/\s+/', $Reflector->Stations[$i]->GetCallSign(), -1, PREG_SPLIT_NO_EMPTY);
    $Callsign = $tmp[0] ?? '';

    // The via node is formatted by CCallsign::GetLogString(), which appends the
    // module only when one is set and an optional " / <suffix>" after it. So the
    // second field is absent for a module-less node, and is the "/" separator when
    // there is a suffix but no module -- neither is a module letter.
    $tmp = preg_split('/\s+/', $Reflector->Stations[$i]->GetVia(), -1, PREG_SPLIT_NO_EMPTY);
    $CallsignSuffix = (isset($tmp[1]) && $tmp[1] !== '/') ? $tmp[1] : '';
	
    // craft payload array
    $payload['stations'][$i] = array(
        'callsign'       => $Callsign,
        'callsignsuffix' => $CallsignSuffix,
        'vianode'        => $Reflector->Stations[$i]->GetVia(),
        'onmodule'       => $Reflector->Stations[$i]->GetModule(),
        'name'           => $Reflector->Stations[$i]->GetName(),
        'lastheard'      => gmdate('Y-m-d\TH:i:sp', $Reflector->Stations[$i]->GetLastHeardTime())
    );

    // position reported by M17 GNSS or D-Star DPRS, rounded to its locator, only present when known
    if ($Reflector->Stations[$i]->GetLocator() != '') {
        $payload['stations'][$i]['locator']   = $Reflector->Stations[$i]->GetLocator();
        $payload['stations'][$i]['latitude']  = (float)$Reflector->Stations[$i]->GetLatitude();
        $payload['stations'][$i]['longitude'] = (float)$Reflector->Stations[$i]->GetLongitude();
    }

    list ($CountryCode, $Country) = $Reflector->GetFlag($Reflector->Stations[$i]->GetCallSign());

    $payload['stations'][$i]['country'] = array (
        'country'     => $Country,
        'countrycode' => $CountryCode
    );

}


// json encode payload array
$records = json_encode($payload);

echo $records;

?>