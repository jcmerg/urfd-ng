<?php

// the dashboard's address as the reflector reports it (DashboardUrl in urfd.ini)
$Host = @parse_url((string)$Reflector->GetDashboardURL(), PHP_URL_HOST);
$Net4 = $Host ? @dns_get_record($Host, DNS_A) : false;
$Net6 = $Host ? @dns_get_record($Host, DNS_AAAA) : false;

/*** add metadata to payload ***/
$payload = array(
    'dashboard_version'  => $PageOptions['DashboardVersion'],
    'ipV4'               => @$Net4[0]['ip'],
    'ipV6'               => @$Net6[0]['ipv6'],
    'reflector_callsign' => str_replace("XLX", "URF", $Reflector->GetReflectorName()),
    'reflector_version'  => $Reflector->GetVersion(),
    'sysop_email'        => $PageOptions['ContactEmail']
);


// json encode payload array
$records = json_encode($payload);

echo $records;

?>