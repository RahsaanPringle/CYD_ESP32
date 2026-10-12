# Work Rant

I am documenting this incident for my future reference while the details remain fresh.

On September 8, HM and IH rolled out a new production server and migrated one of my applications by copying files from the old server. The application was not deployed to the new server through the established YAML deployment process used for the previous server and our other deployments.

After the migration, the application began experiencing production errors. My recommendation was to restore the known-good deployment or roll back to the previous server, then validate the new server’s configuration and get the YAML deployment working before attempting the migration again. This would have restored service while allowing us to investigate the new environment in a controlled manner.

Instead, I was directed to determine where in the application code the production error was occurring. I did not object to reviewing the code or identifying opportunities to make the application more resilient. However, the immediate evidence pointed toward a deployment or configuration difference:

The application had worked on the previous server.
The problems began following the migration.
The migration was performed through a manual file copy rather than the established YAML pipeline.
At least one required environment variable was present on the old server but missing from the new server.

During my conversation with HM, I explained that the missing environment variable was required by the application. HM initially disagreed that the missing variable was contributing to the problem. I was eventually able to get the environment variable added to the new server.

However, adding the variable alone was not sufficient. The existing IIS worker process would not automatically inherit a newly added Windows environment variable. The affected application pool, site, worker process, or IIS service needed to be recycled or restarted so the application could load the updated environment.

HM declined to perform that restart or recycle at that time. As a result, we could not complete a controlled test to determine whether adding the missing environment variable resolved or changed the production behavior.

My frustration is not based simply on a technical disagreement. The larger concern is the sequence of events:

A manual and nonrepeatable migration introduced differences between the working and new environments.
Production errors appeared after that migration.
A known configuration difference was identified.
The corrective configuration was only partially applied because the application process was not restarted.
The focus nevertheless remained on finding a defect in the application code.
I was asked to diagnose the problem while being denied the operational step needed to test the configuration correction.

The technically appropriate response, in my judgment, would have been:

Restore the known-good service through rollback or continued use of the previous server.
Compare the configuration of the old and new servers.
Ensure that all required environment variables and other dependencies were present.
Establish and validate the YAML deployment process for the new server.
Restart or recycle the appropriate IIS process after configuration changes.
Test the application in the corrected environment.
Investigate any remaining application-level issues separately.
Redeploy to production through the validated pipeline.

This incident contributes to my broader concern that I am encountering organizational opposition to my technical recommendations, even when those recommendations are supported by the behavior of the system and by established deployment practices.

I am recording this account as a contemporaneous summary of what occurred, the technical concerns I raised, the actions that were taken or declined, and the reasons for my frustration.