<!DOCTYPE html>
<html>
	<head>
		<meta charset="utf-8">
		<meta name="author" content="Alex Solanos">
		<meta http-equiv="refresh" content="300">
		<link rel="icon" type="image/png" href="favicon.png" />
		<link rel="stylesheet" type="text/css" href="css/main.css" />
		<title>Chios1 Wind Turbine</title>
	</head>
	<body>
		<div id="headerImg">
			<img src="images/head.png">
		</div>
		<div id="mainContent">
			<header>
				<a href="index.html">Home</a>
				<a id="currentA" href="totals.php">Energy</a>
			</header>
			<section id="energyTable">
				
				<?php
				$handle = fopen("images/totals.txt", "r");
				if ($handle) {
					echo"
					<table>
						<thead>
							<tr>
								<th class=\"tableHead\" colspan=\"6\">Energy Production (KWh)</th>
							</tr>
							<tr>
								<th>Current Day</th>
								<th>Current Month</th>
								<th>Current Year</th>
								<th>Last Day</th>
								<th>Last Month</th>
								<th>Last Year</th>
							</tr>
						</thead>
						<tbody>
							<tr>
						";

				    while (($line = fgets($handle)) !== false) {
				        echo "
				        		<td>".$line."</td>";
				    }
				    echo "
				    		</tr>
				    	</tbody>
				    </table>
				    ";
				}
				else
				{
				    echo "
				    <p>
				    	There was an error reading the Energy Production Data.
				    </p>
				    <p>
				    	Please try again later!
				    </p>
				    ";
				}
				?>
				<br>
			</section>
		</div>
	<footer>
		<p>&copy; Designed by <a href="mailto:alexsol.developer@gmail.com">Alex Solanos</a></p>
	</footer>
	</body>
</html>