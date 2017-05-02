#include "condor_common.h"
#include "condor_debug.h"
#include "condor_config.h"
#include "gahp-client.h"
#include "classad_collection.h"

#include "annex.h"
#include "annex-setup.h"
#include "generate-id.h"
#include "user-config-dir.h"

#include "Functor.h"

#include "CreateKeyPair.h"
#include "CheckForStack.h"
#include "CreateStack.h"
#include "FunctorSequence.h"
#include "GenerateConfigFile.h"
#include "SetupReply.h"
#include "WaitForStack.h"

extern ClassAdCollection * commandState;

void
checkOneParameter( const char * pName, int & rv, std::string & pValue ) {
	param( pValue, pName );
	if( pValue.empty() ) {
		if( rv == 0 ) { fprintf( stderr, "Your setup is incomplete:\n" ); }
		fprintf( stderr, "\t%s is unset.\n", pName );
		rv = 1;
	}
}

void
checkOneParameter( const char * pName, int & rv ) {
	std::string pValue;
	checkOneParameter( pName, rv, pValue );
}

int
check_account_setup( const std::string & publicKeyFile, const std::string & privateKeyFile, const std::string & cfURL, const std::string & ec2URL ) {
	ClassAd * reply = new ClassAd();
	ClassAd * scratchpad = new ClassAd();

	std::string commandID;
	generateCommandID( commandID );

	Stream * replyStream = NULL;

	EC2GahpClient * cfGahp = startOneGahpClient( publicKeyFile, cfURL );
	EC2GahpClient * ec2Gahp = startOneGahpClient( publicKeyFile, ec2URL );

	std::string bucketStackName = "HTCondorAnnex-ConfigurationBucket";
	std::string bucketStackDescription = "configuration bucket";
	CheckForStack * bucketCFS = new CheckForStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		bucketStackName, bucketStackDescription,
		commandState, commandID );

	std::string lfStackName = "HTCondorAnnex-LambdaFunctions";
	std::string lfStackDescription = "Lambda functions";
	CheckForStack * lfCFS = new CheckForStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		lfStackName, lfStackDescription,
		commandState, commandID );

	std::string rStackName = "HTCondorAnnex-InstanceProfile";
	std::string rStackDescription = "instance profile";
	CheckForStack * rCFS = new CheckForStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		rStackName, rStackDescription,
		commandState, commandID );

	std::string sgStackName = "HTCondorAnnex-SecurityGroup";
	std::string sgStackDescription = "security group";
	CheckForStack * sgCFS = new CheckForStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		sgStackName, sgStackDescription,
		commandState, commandID );

	SetupReply * sr = new SetupReply( reply, cfGahp, ec2Gahp, "Your setup looks OK.\n", scratchpad,
		replyStream, commandState, commandID );

	FunctorSequence * fs = new FunctorSequence(
		{ bucketCFS, lfCFS, rCFS, sgCFS }, sr,
		commandState, commandID, scratchpad );

	int setupTimer = daemonCore->Register_Timer( 0, TIMER_NEVER,
		 (void (Service::*)()) & FunctorSequence::operator(),
		 "CheckForStacks", fs );
	cfGahp->setNotificationTimerId( setupTimer );
	ec2Gahp->setNotificationTimerId( setupTimer );

	return 0;
}

int
check_setup() {
	int rv = 0;

	std::string accessKeyFile, secretKeyFile, cfURL, ec2URL;
	checkOneParameter( "ANNEX_DEFAULT_ACCESS_KEY_FILE", rv, accessKeyFile );
	checkOneParameter( "ANNEX_DEFAULT_SECRET_KEY_FILE", rv, secretKeyFile );
	checkOneParameter( "ANNEX_DEFAULT_CF_URL", rv, cfURL );
	checkOneParameter( "ANNEX_DEFAULT_EC2_URL", rv, ec2URL );

	checkOneParameter( "ANNEX_DEFAULT_S3_BUCKET", rv );
	checkOneParameter( "ANNEX_DEFAULT_ODI_SECURITY_GROUP_IDS", rv );
	checkOneParameter( "ANNEX_DEFAULT_ODI_LEASE_FUNCTION_ARN", rv );
	checkOneParameter( "ANNEX_DEFAULT_SFR_LEASE_FUNCTION_ARN", rv );
	checkOneParameter( "ANNEX_DEFAULT_ODI_INSTANCE_PROFILE_ARN", rv );

	if( rv != 0 ) {
		return rv;
	} else {
		return check_account_setup( accessKeyFile, secretKeyFile, cfURL, ec2URL );
	}
}

void
setup_usage() {
	fprintf( stdout,
		"\n"
		"To do the one-time setup for an AWS account:\n"
		"\tcondor_annex -setup\n"
		"\n"
		"To specify the files for the access (public) key and secret (private) keys:\n"
		"\tcondor_annex -setup\n"
		"\t\t<path/to/access-key-file>\n"
		"\t\t<path/to/private-key-file>\n"
		"\n"
		"Expert mode (to specify the region, you must specify the key paths):\n"
		"\tcondor_annex -aws-ec2-url https://ec2.<region>.amazonaws.com\n"
		"\t\t-setup <path/to/access-key-file>\n"
		"\t\t<path/to/private-key-file>\n"
		"\t\t<https://cloudformation.<region>.amazonaws.com/>\n"
		"\n"
	);
}

int
setup( const char * pukf, const char * prkf, const char * cloudFormationURL, const char * serviceURL ) {
	std::string publicKeyFile, privateKeyFile;
	if( pukf != NULL && prkf != NULL ) {
		publicKeyFile = pukf;
		privateKeyFile = prkf;
	} else {
		std::string userDirectory;
		if(! createUserConfigDir( userDirectory )) {
			fprintf( stderr, "You must therefore specify the public and private key files on the command-line.\n" );
			setup_usage();
			return 1;
		} else {
			publicKeyFile = userDirectory + "/publicKeyFile";
			privateKeyFile = userDirectory + "/privateKeyFile";
		}
	}

	int fd = safe_open_no_create_follow( publicKeyFile.c_str(), O_RDONLY );
	if( fd == -1 ) {
		fprintf( stderr, "Unable to open public key file '%s': '%s' (%d).\n",
			publicKeyFile.c_str(), strerror( errno ), errno );
		setup_usage();
		return 1;
	}
	close( fd );

	fd = safe_open_no_create_follow( privateKeyFile.c_str(), O_RDONLY );
	if( fd == -1 ) {
		fprintf( stderr, "Unable to open private key file '%s': '%s' (%d).\n",
			privateKeyFile.c_str(), strerror( errno ), errno );
		setup_usage();
		return 1;
	}
	close( fd );


	std::string cfURL = cloudFormationURL ? cloudFormationURL : "";
	if( cfURL.empty() ) {
		// FIXME: At some point, the argument to 'setup' should be the region,
		// not the CloudFormation URL.
		param( cfURL, "ANNEX_DEFAULT_CF_URL" );
	}
	if( cfURL.empty() ) {
		fprintf( stderr, "No CloudFormation URL specified on command-line and ANNEX_DEFAULT_CF_URL is not set or empty in configuration.\n" );
		return 1;
	}

	std::string ec2URL = serviceURL ? serviceURL : "";
	if( ec2URL.empty() ) {
		param( ec2URL, "ANNEX_DEFAULT_EC2_URL" );
	}
	if( ec2URL.empty() ) {
		fprintf( stderr, "No EC2 URL specified on command-line and ANNEX_DEFAULT_EC2_URL is not set or empty in configuration.\n" );
		return 1;
	}

	ClassAd * reply = new ClassAd();
	ClassAd * scratchpad = new ClassAd();

	// This 100% redundant, but it moves all our config-file generation
	// code to the same place, so it's worth it.
	scratchpad->Assign( "AccessKeyFile", publicKeyFile );
	scratchpad->Assign( "SecretKeyFile", privateKeyFile );

	std::string commandID;
	generateCommandID( commandID );

	Stream * replyStream = NULL;

	EC2GahpClient * cfGahp = startOneGahpClient( publicKeyFile, cfURL );
	EC2GahpClient * ec2Gahp = startOneGahpClient( publicKeyFile, ec2URL );

	// FIXME: Do something cleverer for versioning.
	std::string bucketStackURL = "https://s3.amazonaws.com/condor-annex/bucket-7.json";
	std::string bucketStackName = "HTCondorAnnex-ConfigurationBucket";
	std::string bucketStackDescription = "configuration bucket (this takes less than a minute)";
	std::map< std::string, std::string > bucketParameters;
	CreateStack * bucketCS = new CreateStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		bucketStackName, bucketStackURL, bucketParameters,
		commandState, commandID );
	WaitForStack * bucketWFS = new WaitForStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		bucketStackName, bucketStackDescription,
		commandState, commandID );

	// FIXME: Do something cleverer for versioning.
	std::string lfStackURL = "https://s3.amazonaws.com/condor-annex/template-7.json";
	std::string lfStackName = "HTCondorAnnex-LambdaFunctions";
	std::string lfStackDescription = "Lambda functions (this takes about a minute)";
	std::map< std::string, std::string > lfParameters;
	lfParameters[ "S3BucketName" ] = "<scratchpad>";
	CreateStack * lfCS = new CreateStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		lfStackName, lfStackURL, lfParameters,
		commandState, commandID );
	WaitForStack * lfWFS = new WaitForStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		lfStackName, lfStackDescription,
		commandState, commandID );

	// FIXME: Do something cleverer for versioning.
	std::string rStackURL = "https://s3.amazonaws.com/condor-annex/role-7.json";
	std::string rStackName = "HTCondorAnnex-InstanceProfile";
	std::string rStackDescription = "instance profile (this takes about two minutes)";
	std::map< std::string, std::string > rParameters;
	rParameters[ "S3BucketName" ] = "<scratchpad>";
	CreateStack * rCS = new CreateStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		rStackName, rStackURL, rParameters,
		commandState, commandID );
	WaitForStack * rWFS = new WaitForStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		rStackName, rStackDescription,
		commandState, commandID );

	// FIXME: Do something cleverer for versioning.
	std::string sgStackURL = "https://s3.amazonaws.com/condor-annex/security-group-7.json";
	std::string sgStackName = "HTCondorAnnex-SecurityGroup";
	std::string sgStackDescription = "security group (this takes less than a minute)";
	std::map< std::string, std::string > sgParameters;
	CreateStack * sgCS = new CreateStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		sgStackName, sgStackURL, sgParameters,
		commandState, commandID );
	WaitForStack * sgWFS = new WaitForStack( reply, cfGahp, scratchpad,
		cfURL, publicKeyFile, privateKeyFile,
		sgStackName, sgStackDescription,
		commandState, commandID );

	CreateKeyPair * ckp = new CreateKeyPair( reply, ec2Gahp, scratchpad,
		ec2URL, publicKeyFile, privateKeyFile,
		commandState, commandID );

	GenerateConfigFile * gcf = new GenerateConfigFile( cfGahp, scratchpad );

	SetupReply * sr = new SetupReply( reply, cfGahp, ec2Gahp, "Setup successful.\n", scratchpad,
		replyStream, commandState, commandID );

	FunctorSequence * fs = new FunctorSequence(
		{ bucketCS, bucketWFS, lfCS, lfWFS, rCS, rWFS, sgCS, sgWFS, ckp, gcf }, sr,
		commandState, commandID, scratchpad );


	int setupTimer = daemonCore->Register_Timer( 0, TIMER_NEVER,
		 (void (Service::*)()) & FunctorSequence::operator(),
		 "CreateStack, DescribeStacks, WriteConfigFile", fs );
	cfGahp->setNotificationTimerId( setupTimer );
	ec2Gahp->setNotificationTimerId( setupTimer );

	return 0;
}